# Installer: bash wizard and Go assistant

Two programs on the user's PC install and manage Akam, with the same command names and the same state files:

- `./akam` (bash, `tools/wizard/`, root of the repository): the **reference** implementation, Linux only.
- `akam` (Go, `tools/assistant/`): one static binary with no runtime dependency, for six targets. Built-in SSH, control
  port client, RTSP talk client, G.711 and WAV, a full-screen interface, and an experimental native image builder.
  Select it from a checkout with `AKAM_USE_BIN=1 ./akam ARGS...` (never run both at once on the same state).

This file covers the commands, the state, configs, install paths, backup and flash rules, safety gates and the platform
status. The release side is in [kit-and-release.md](kit-and-release.md).

Status: the bash wizard **hardware-validated** on Linux for first install, backup, flash through the maintenance card and
flash over SSH, on two cameras. The Go assistant: **hardware-validated in part** (Linux amd64: SSH, control-port options,
`video show`, `led`, `ntp`, `talk`; an earlier Windows build: discovery, options over SSH, talk, clock offer);
**host-tested only**: update, restore, telnet backup and flash, the SD writer, first install, configs, per-camera keys and
the firmware chooser; **experimental**: the native image builder, all Windows, macOS and arm64 binaries.

## 1. Commands

Both programs accept these commands (the Go assistant adds the ones marked G). With no command, the bash wizard opens a
step menu and the Go assistant opens a home screen. Every command that changes the camera accepts `--dry-run`.

| Command | What it does |
|---|---|
| `doctor` | Checks this computer's tools. Changes nothing |
| `get-kit` | Downloads and verifies the release kit (section 5 of [kit-and-release.md](kit-and-release.md)) |
| `build` | Builds from source instead (optional, needs more tools) |
| `prepare-backup-sd` | Prepares the microSD card used for the backup (rescue shell) |
| `find-camera` | Finds the camera, asks its address, checks ports |
| `backup` | Backs up all 8 partitions (section 5) |
| `image` | Builds the Akam APP image from the backup and the kit |
| `prepare-flash-sd` | Prepares the card that carries the image and the SSH key |
| `flash` | Writes the image (SSH when Akam runs, else maintenance telnet) |
| `verify` | Checks SSH, ports and hardware, then again without the card |
| `update` | Flashes a new image over SSH, no card |
| `restore` | Puts the original APP back from the stock backup |
| `wipe-sd` | Zero-fills a card that holds a Wi-Fi password |
| `hostname [NAME]` | Network name, applied at the next power cycle |
| `rtsp-auth [set\|disable\|status]` | Digest login on the stream. Password typed on the PC, never shown |
| `led [show \| status on\|off\|auto] [ir auto\|off]` | Front LED and IR LEDs |
| `audio [show \| volume N \| beep \| on \| off \| talk on\|off \| mic on\|off]` | Speaker and microphone |
| `picture [show \| brightness N \| contrast N \| saturation N \| sharpness N \| wb conf\|manual \| night auto\|day\|night \| reset]` | Picture. Sliders are saved; `wb` and `night` follow the camera's own rules ([image-day-night-leds.md](image-day-night-leds.md)) |
| `video [show \| bitrate N \| fps N \| gop N]` | Encoder settings; asks (default No) whether to restart the video service |
| `debug [on [MIN] \| off \| status]` | The 8091 debug flag |
| `ntp [status \| set SERVER... \| off]` | Opt-in time sync |
| `onvif [enable [PORT] \| disable \| status]` | ONVIF; `enable` asks first and needs the stream password |
| `mqtt [status \| set \| disable \| forget \| test]` | MQTT client |
| `talk [SECONDS \| test]`, `say FILE` | Speak or play audio on the camera (backchannel; needs the stream password) |
| `status`, `step NAME`, `steps` (G) | Step status; rerun one step |
| `cameras`, `rename-camera NEW`, `register` | Several cameras; record a camera that already runs Akam |
| `key [show \| use-shared \| use-new \| use FILE.pub]` (G) | SSH key of one camera (see [network-time-security.md](network-time-security.md)) |
| `config [show \| save-global \| save-local \| apply \| edit-global \| forget-secrets]` (G) | Saved settings (section 3) |
| `releases` (G) | Lists the published versions, newest first (`--include-prereleases`) |
| `forget-host-key IP` (G) | Forgets a pinned SSH host key (asks first) |
| `licenses`, `version`, `self-test` (G) | Licences of the built-in software; version; offline checks |

Global options: `--dry-run`, `--kit FILE`, `--version X.Y.Z`, `--allow-unsigned`, `--expect-sha256 H`, `--backup-dir DIR`,
`--backup-copy DIR`, `--ip ADDRESS`, `--identity FILE`, `--image FILE`, `--stock`, `--manual-telnet`, `--out DIR`,
`--comp xz|gzip`, `--allow-unknown-stock-comp`, `--builder native|tools` (G), `--i-understand-untested`, `--device DEV`,
`--pubkey FILE`, `--ssid NAME`, `--camera NAME`, `--state-dir DIR`, `--no-color`, `--plain`.

The nine setup steps have stable names: `doctor`, `get-kit` (or `build`), `prepare-backup-sd`, `find-camera`, `backup`,
`image`, `prepare-flash-sd`, `flash`, `verify`. Each step shows What, Why, Risk (none, writes to the SD card, writes to the
camera flash) and the exact command before running it. A done step asks "already done, redo?" (default No) and is
rechecked against the disk (a vanished image makes its step "todo" again).

The camera-side helper scripts the commands run over SSH are listed in [architecture.md](architecture.md) and in the
specs of the features they manage. The installers MUST NOT send a command shape the camera scripts do not accept.

## 2. State

| Item | Value |
|---|---|
| State directory | `AKAM_STATE_DIR`, else `--state-dir`; default Linux `$XDG_STATE_HOME/akam` or `~/.local/state/akam`; macOS `~/Library/Application Support/Akam`; Windows `%LOCALAPPDATA%\Akam`. Mode 0700 |
| Per camera | `<state>/cameras/<name>/`: `state`, `akam.log`, `reflash.log`, `reflash-precheck.log`, `camera.conf`, `camera.secrets` |
| Selection | `<state>/current_camera` (one name) |
| Rehearsal | `state.dry` instead of `state` under `--dry-run` |
| Format | `KEY=VALUE` lines, sourceable, never a secret, never multi-line. A state write that would store a secret is refused |
| Camera name | `a-z`, `0-9`, `-`, at most 20 characters; used in backup and image folder names (never a name the camera reported) |
| SSH pins | Go: `<state>/known_hosts` (Akam's own file) |
| Log | `akam.log` records each command before it runs, with secrets replaced by `***`. Shell tracing is forced off |
| Compatibility | The Go and bash programs read and write the same files. Progress made before camera names existed is the camera `default` |

Each step is checked against the state and the disk. Progress is resumable.

## 3. Configs (Go assistant only)

Settings of a camera saved on this computer, so a new camera can be given the same options. **Host-tested only.**

| File | Content |
|---|---|
| `<state>/profiles/global.conf` | Global template for new cameras |
| `<state>/profiles/global.secrets` | Passwords and Wi-Fi of the template (optional, opt-in) |
| `<camera dir>/camera.conf` | What was last read from or applied to that camera |
| `<camera dir>/camera.secrets` | Its passwords and Wi-Fi (optional, opt-in) |

Format: first non-comment line `akam_config_version=1` (a newer version is refused, never half read); one `key=value`
per line, LF only, no quoting, no control characters, at most 512 bytes per line and 64 KiB per file; a key appears once;
a known key with an invalid value is an error and nothing is applied; unknown keys are kept on rewrite and ignored
otherwise. Files are written atomically with private modes.

Keys: the control-port option ids (`video.bitrate_cfg`, `video.fps_cfg`, `video.gop_cfg`, `isp.*`, `led.status`,
`night.ir_led`, `audio.*`, ...) plus `onvif.enabled`, `onvif.port`, `ntp.enabled`, `ntp.servers` (1 to 3, comma
separated), `mqtt.enabled`, `mqtt.host`, `mqtt.port`, `mqtt.user`, `mqtt.base_topic`, `mqtt.client_id`, `mqtt.discovery`,
`mqtt.discovery_prefix`, `mqtt.interval`, `auth.enabled`, `auth.user`, `key.kind` (`shared`, `own`, `custom`), `key.path`,
`saved.from`, `saved.at`. The secrets file holds `auth.password`, the MQTT password and the Wi-Fi.

Rules: secrets are saved only after an explicit yes (default No) at each save. `apply` compares with the camera, shows the
differences, asks (default No), then sends them as the option screens do; the video restart is asked separately; a
password is sent only with the setting that needs it (it cannot be read back); Wi-Fi is never applied to a running camera
(it only prepares the card of a new one); the SSH key change and a removal of the stream password each ask their own
question. `forget-secrets` deletes the saved secrets; nothing is ever synchronised off the computer.

## 4. Install paths

### 4.1 First install from the stock firmware (no UART needed)

1. **Rescue card** (`prepare-backup-sd`, `tools/sd/prepare_first_boot_sd.sh`): a microSD card carrying a pinned third-party
   toolkit (`lsc-tuya-toolkit`, fixed commit and tree hash, verified) configured for offline use: Telnet on port 24 as
   root, Wi-Fi from `wifi.conf` (the password is in clear on the card), a `hack.sh` hook, `dump.done`. The camera boots the
   stock firmware, runs the hook from the card and offers a root BusyBox shell on telnet port 24.
2. **Find the camera** (`find-camera`): the address is asked or scanned (vendor MAC prefix, the camera ports).
3. **Backup** over that shell (section 5).
4. **Image** built on the PC from the backup and the kit (section 6).
5. **Flash card** (`prepare-flash-sd`): the card gets the toolkit, an empty `AKAM_MAINTENANCE` at its root (keeps the
   maintenance Telnet alive and enables the SD hooks), `akam/authorized_keys` with the user's public key, the image,
   `flash_tool`, `flash_guard.sh`, `install_with_flash_tool.sh` and `preflight_check.sh`, and **no `*.bin` at the root**
   (the stock updater would flash it).
6. **Flash** (section 7.1), power cycle by hand, then **verify** (SSH by key, hardware check, ports 22, 554 and 8091 only).
7. Remove the card and power cycle again; `verify` repeats the checks without the card.

The card writer refuses anything that is not a whole, removable (or SD/MMC), plausibly sized disk (128 MiB to 128 GiB)
and never a disk that holds the system or a mounted repository or home. The device is confirmed by model, size and name,
in a red box with one yes/no (default No). Linux uses `sudo` or `pkexec`; Windows asks UAC once and talks to a small helper
through a private named pipe; macOS uses `sudo` (an `osascript` dialog only with `AKAM_SD_ELEVATE=osascript`). The helper
does only `list`, `prepare`, `verify` and `wipe`, with the request on stdin or a private channel, never in an argument.

### 4.2 Telnet-only route

When the card route is not wanted, the wizard drives the camera's maintenance Telnet shell itself: a scripted session on
`/dev/tcp` with an end-of-command marker per command (`cmd; echo "__AKAM_DONE_N:$?__"`), Telnet negotiation bytes
filtered, one command per send (under 400 bytes). It falls back to printing each command for the user to paste
(`--manual-telnet`, or automatically after 2 failures). The flash confirmation (`YES`) is typed by the human and relayed;
the code never sends it.

### 4.3 Camera that already runs Akam

`register` records it without redoing backup, image or flash. `update` flashes over SSH (section 7.2). `backup` of an Akam
camera streams the partitions over SSH.

## 5. Backup

- **Where.** Bash: `~/akam-backup-<name>-<date>/`. Go: `<Documents>/Akam backups/<camera>/<date>[-N]` (Linux XDG Documents,
  macOS and Windows Documents), falling back to `~/Akam backups` when Documents is in OneDrive, iCloud Drive or a similar
  sync folder. `--backup-dir DIR` accepts a folder made by hand. The folder is never inside the repository or on the card
  being written, must be new or empty, and must have 64 MiB plus the dump size free.
- **Contents.** `mtdblock0.bin` to `mtdblock7.bin`, `SHA256SUMS`, `README.txt`, `camera_info.txt`.
- **Expected sizes** (checked): 204800, 4096, 4096, 65536, 1572864, 1032192, 262144, 5242880 bytes.
- **Double read.** Each partition is read twice (`dd`) on the camera, both copies must have the same size and `sha256`,
  and the copy on the PC must match. For an Akam camera over SSH the two reads are streamed to the PC and compared; the
  folder carries a marker `kind=akam`. **A backup of an Akam camera is not a way back to stock** and is never used to
  restore stock; only a backup taken from the stock camera is a stock backup.
- **Transfer.** The camera listens (`nc -l -p 8766 < dump` for the PC to read, `-p 8765` to receive) and the PC connects
  outbound with `/dev/tcp`, so a PC firewall that blocks inbound traffic does not matter; size and hash are compared;
  3 tries. When the camera's `nc` has no `-l`, the card route is used.
- **Privacy.** The folder is created with `umask 077` (0700, files 0600; on Windows a protected ACL), also for imports and
  copies. The CONFIG dump contains the Wi-Fi password in clear, the stream password hash and the clear-text ONVIF
  password. The wizard prints a "contains secrets" warning. A medium without permissions (FAT, exFAT, network share) shows
  "Private: no" and asks one yes/no (default No).
- **Second copy.** The wizard asks the user to copy the folder to a second medium before continuing (it is the only way
  back to the original firmware). `--backup-copy DIR` copies it and re-checks every hash.
- Backups are never published or committed. Images derived from them are not either.

## 6. Image build

Inputs: the user's backup (`mtdblock7.bin` and the kernel name from `mtdblock4.bin`) and a verified kit.
Bash path: `tools/image/assemble_from_kit.sh` unpacks `mtdblock7.bin` with `unsquashfs` as a normal user, then runs
`src/ak_rtsp/build_firmware.sh --kit`. Steps and gates:

1. Verify the kit ([kit-and-release.md](kit-and-release.md)). A `kit_format` the installer does not know
   (`tools/wizard/kit_compat.txt`, currently `1`) is refused.
2. Fingerprint the stock APP (compressor, block size, BCJ filter, sha256) and compare with
   `tools/release/supported_hardware.txt`: a record marked `validated` continues; anything else is "untested" and needs
   an explicit yes (`--i-understand-untested`). The whole `/proc/mtd` layout must match; a different one is always
   refused.
3. Copy the tree, remove `anyka_ipc`, install the Akam files from `image_files.txt`, patch `service.sh`, `ethernet.sh`,
   `drop_caches.sh` and `main.sh`.
4. Safety checks (modes, no private key anywhere), `mksquashfs` with `-all-root` and the **same compressor as the stock
   APP** (xz and gzip only; another compressor needs `--allow-unknown-stock-comp`), round-trip verify, size guard before
   padding (an image within one block of the partition size needs `AKAM_ALLOW_TIGHT=1`), pad with zero bytes to exactly
   **5,242,880 bytes**, write `<name>.sha256` and `build_report.txt` (free space).
5. Device nodes in the stock tree cannot be created without root: the script then refuses and asks to run as root.

Output file name `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs`. The stock files are proprietary: the work tree stays on the
user's machine.

**Native builder (Go, experimental).** `akam image --builder native` builds the same tree and squashfs with built-in
reader and writer. The native result is about 13 % larger (Go xz encoder) and still fits. A real stock backup and a fresh
kit gave the same tree (300 entries) as the bash builder; a native image was loop-mounted read-only on a camera and all
files read back identical. It was **never flashed**, so it is opt-in. On Windows and macOS the interim rule is to build
the image on Linux or WSL and pass it with `--image FILE`; the assistant checks the size and records the md5.

## 7. Flash

`flash_tool` (`src/ak_rtsp/flash_tool.c`, an ARM binary from the kit) writes the APP partition through the raw
`/dev/mtd7` character device, reading back every 4 KB erase block and retrying a mismatch. A plain `dd` to
`/dev/mtdblock7` corrupts writes through DMA contention with the SPI-NOR controller while Wi-Fi, camera or ISP drivers
are active and MUST NOT be used.

### 7.1 Through the maintenance card (stock firmware, or UART)

`install_with_flash_tool.sh` runs from the card, picks the newest `ak_rtsp_firmware_*.squashfs` by file name (the camera
has no clock, so file times mean nothing), checks that it is exactly 5,242,880 bytes, runs `flash_tool`, and reads the
partition back for an md5 check before the user is told to power-cycle. The human types `YES`.

### 7.2 Over SSH, no card, no UART (`update`)

Only for a camera that already runs Akam. On an Akam image `/usr` is the APP squashfs, so the running Dropbear, watchdog
and scripts are paged in from the partition being written. The flash therefore runs entirely from RAM:

| Camera step (`akam_reflash_ssh.sh`) | Action |
|---|---|
| `precheck` | `mtd7` is APP `00500000`; enough free RAM in `/tmp` (10240 KB). Read-only |
| `prepare --md5 HEX` | The image is 5,242,880 bytes with that md5; BusyBox, Dropbear, `flash_tool` and the image are copied into `/tmp/akam_reflash`; a **second Dropbear** starts from RAM on port 2222 (same host key and keys) so the operator keeps a shell that does not depend on `/usr` |
| `start --md5 HEX --i-understand-no-uart` | Starts the flash worker detached (`setsid`, own session, no SSH environment, stdio to a log), from the RAM copies. It re-checks `mtd7` and the image size before the erase. 3 write-and-verify attempts |
| `status` | Worker state and log tail |
| `verify --md5 HEX` | Reads `/dev/mtd7` back and compares (exit 0 only on a match) |

`flash_guard.sh` is sourced by every script that writes the APP. `akam_flash_guard` refuses inside an SSH session or when
the script or tool lives under `/usr` on a squashfs from `mtdblock7` (override `AKAM_FLASH_I_KNOW=1`, meant for the stock
firmware over Telnet). The detached variant for the SSH route ignores that override and allows the write only when the
operator accepted `--i-understand-no-uart` (a half-written APP is recoverable only through UART); script, tool and image
resolve under the RAM directory, none under `/usr`, and that directory is a tmpfs or ramfs mount; and the process is
detached from the SSH session (no SSH variables, no `dropbear` among its ancestors, session leader). `restore.sh` refuses
the same way.

The Go flash path (`internal/flash`) is the one audited place that can write a camera's flash. It needs a Confirmation
that only an explicit yes (default No, refused without a terminal, bound to one image md5) can create; it checks the
size and md5 on the PC, the sha256 on the camera after upload, the md5 again in `prepare`, once more just before the
erase, and after the write requires **two independent read-backs** (the camera script's and the assistant's) to equal the
image md5 before it reports success. Its command set is closed (a test checks every command the fake camera receives).
A failure after the flash started always says **DO NOT power-cycle**. Closing the window does not stop the camera-side
worker.

### 7.3 Common rules

- **A verified stock backup gates every flash.** The recorded backup folder must exist and its `SHA256SUMS` is re-hashed
  right before. A camera without a recorded stock backup is offered one first.
- `preflight_check.sh` runs before the write and again before the card is removed: `/etc/config/wpa_supplicant.conf`
  non-empty with a network block, an `ssid` and a `psk` (or an open network), `/proc/mtd` lists APP. Without
  Wi-Fi configuration Akam would boot with no network; `--ensure-wifi` writes the file on the camera from the card's own
  Wi-Fi config (the password never travels through the PC; refused for quotes, backslashes, line breaks, a WPA password
  outside 8 to 63 characters, or a security mode other than WPA-PSK or open).
- The program never reboots or power-cycles the camera. After a flash it tells the user to unplug and replug USB-C by hand.
  Software `reboot` is unreliable on this hardware.
- The only camera process it ever stops is the video service, behind one yes/no (default No) after a video change.
- `sudo` is never run by the wizard on its own; it prints the command.
- Every destructive step (erase a card, write the flash) needs an explicit yes, default No; Enter, Esc and `q` mean No;
  without a terminal the step is refused. `--dry-run` rehearses everything and changes nothing.
- A camera that is not a validated fingerprint shows a warning and needs another explicit yes.

## 8. Restore to stock

`restore` writes the stock APP (`mtdblock7.bin` of the user's stock backup, 5,242,880 bytes) back with the same
guards. It leaves bootloader, kernel, root file system and CONFIG alone. `restore.sh` (card route) MUST run from a UART
`init=/bin/sh` shell or the maintenance Telnet of the stock firmware. The Go assistant exposes it as `restore` and
`update --stock`. **Never rehearsed on a camera.** After a flash with no boot and no SSH the recovery tree is: wait,
check card and address, remove the card, use the UART, then `restore`. SSH never validated means the wizard does not say
"done" and does not offer to remove the card (the maintenance Telnet is the safety net).

## 9. Firmware choice (Go assistant, host-tested only)

`update` asks "Which firmware?" or takes `--version X.Y.Z` (a published release from GitHub), `--kit FILE`, `--image FILE`
or `--stock`. `releases` lists versions newest first (prereleases hidden unless `--include-prereleases`). A downgrade asks
a question. The downloader uses a small TLS client (not `net/http`). The stock choice is the restore flow.
No release was published when this was written, so the chooser was only tested against a test server that plays GitHub.

## 10. Home screen and interface (Go)

- Entries: set up a new camera (the nine steps), update this camera, camera options (LED and night, picture, video, audio,
  network: ONVIF, NTP, name, Wi-Fi shown read-only; security; advanced), back up and restore, find cameras, settings
  (backup folder, state folder, SD helper mode, licences), check this computer.
- Option screens read all values in one go, accept edits locally ("Pending"), and send them only on `apply`. Every option
  says whether it is saved on the camera and when it applies (now, next video restart, next reboot). An option an old image
  does not know is shown as "not available on this image (update first)".
- **Camera chooser.** Known cameras are shown at once and probed in parallel. Further sources: a name (DNS then
  `<name>.local`), ONVIF WS-Discovery, and a scan of the local /24 on ports 22, 24, 554 and 8091 only. The two searches
  that send packets each ask first (default No) and show range and ports. A camera answering on 8091 is identified read-only
  (`GET fw.version`, `GET sys.hostname`). Names a camera reports are cleaned before display. VPN, point-to-point and virtual
  adapters are not scanned.
- `--plain`, `AKAM_PLAIN=1` or no terminal gives line-based output and prompts. Colours follow `NO_COLOR` and the terminal.

## 11. Supported host platforms

| Host | Bash wizard | Go assistant |
|---|---|---|
| Arch Linux x86-64 | Supported, tested | linux/amd64: partly tested on real cameras |
| Debian 12+, Ubuntu 22.04+ x86-64 | Supported, partly tested | same binary |
| Linux arm64 | Not tested | built, not hardware-tested |
| macOS (Intel and Apple Silicon) | Not supported by the wizard | darwin/amd64 and darwin/arm64: built and host-tested, **not hardware-tested**; the SD helper and `osascript` path were never run |
| Windows | Not supported (use WSL2 as Debian/Ubuntu) | windows/amd64: an earlier build tested on a real camera (discovery, SSH options, talk, clock offer); the current interface build, the SD writer, UAC flow and flash are not tested. windows/arm64: built only |

The binaries are not signed: Windows SmartScreen and macOS Gatekeeper warn. The user compares the SHA-256 with the signed
`SHA256SUMS` first. Tools the bash wizard needs: `curl`, `tar`, `xz`, `squashfs-tools`, `ssh`, `ssh-keygen`, and the SD
tools (`util-linux`, `dosfstools`, `parted`); to build from source also `make git bzip2 file shellcheck`, the pinned Zig
and an ARM disassembler. The Go assistant needs nothing at run time except `ffmpeg` for microphone capture on macOS, and
`unsquashfs`/`mksquashfs` for the interim image rule.

## 12. Tests

`tests/wizard_core_test.sh`, `wizard_camera_test.sh`, `wizard_e2e_test.sh`, `wizard_tui_pty_test.sh`,
`wizard_tui_widgets_test.sh`, `camsh_test.sh`, `transfer_test.sh`, `sd_card_test.sh`, `first_boot_sd_test.sh`,
`build_from_kit_test.sh`, `build_firmware_test.sh`, `reflash_ssh_test.sh`, `flash_guard_test.sh`, `preflight_test.sh`,
`akam_authkeys_test.sh`, `assistant_test.sh` (and `go test ./...` under `tools/assistant`), `readme_links_test.sh`.
`tools/wizard/fake_camera.sh` plays a camera for the wizard tests.
