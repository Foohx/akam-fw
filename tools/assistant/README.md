# akam: the assistant as a single binary (experimental, phases P1 to P3)

`akam` is the Go version of the guided setup program (`./akam`, bash, in the repository root). It is one
static executable with no runtime dependency: SSH, the control port, the RTSP talk client, G.711 and
WAV are built in. The plan is in [specs/09-spec-assistant-binary.md](../../specs/09-spec-assistant-binary.md).

**Status: phases P1 to P3 and phase 11 (cross-platform), experimental.** The binary does the daily commands on a
camera that already runs Akam, gets and verifies the kit, updates or restores a camera over SSH, does the whole first
install from a stock camera (telnet detection, backup, built-in microSD writer, flash), and since phase 11 it is built
for six targets (Linux, Windows and macOS, amd64 and arm64) and has a home screen, a camera chooser, option screens, a
backup over SSH and a guided update. **The bash wizard (`./akam`) remains the reference on Linux.** The program never
reboots or power-cycles a camera: after a flash it tells you to do it by hand. Evidence: host tests on fake cameras and
files, plus a few real-camera checks of the SSH and 8091 paths (see [testing status](../../docs/testing-status.md)).
**Nothing was run on a real Windows or macOS computer**, and no Windows or macOS card write has been tried.
Details per OS are in "Phase 11" just below.

**Self-contained.** The camera-side scripts the program uploads (`akam_reflash_ssh.sh`, `flash_guard.sh`,
`preflight_check.sh`, `install_with_flash_tool.sh`, `restore.sh`, `akam_hwcheck.sh`) and the supported-hardware list
are embedded in the binary (`internal/assets`, copied from their source paths by `go generate ./internal/assets`; a
test fails when a copy drifts). The program no longer looks for a source checkout: it works from any directory, and a
folder it happens to be run in can never provide what is sent to a camera. `flash_tool` (an ARM binary) comes only from
the verified kit.

## Phase 11: one binary for Linux, Windows and macOS (experimental)

Plan: [specs/11-spec-assistant-cross-platform.md](../../specs/11-spec-assistant-cross-platform.md). The release has six
binaries: `akam-linux-amd64`, `akam-linux-arm64`, `akam-darwin-amd64`, `akam-darwin-arm64`, `akam-windows-amd64.exe` and
`akam-windows-arm64.exe`. **All of them are experimental. The bash wizard (`./akam`) stays the reference on Linux.**
Only the Linux amd64 paths have been exercised on a real camera, and only in part; the Windows and macOS binaries are
**built and host-tested, not hardware-tested** (see [testing status](../../docs/testing-status.md) and the
[hardware test plan](../../docs/assistant_hardware_test_plan.md)). Download, check and first run:
[docs/assistant_install.md](../../docs/assistant_install.md).

### Home screen

`akam` with no command opens the home screen (on a terminal: arrows or `j`/`k`, Enter, `?` help, `c` camera, `q`; with
`--plain`, `AKAM_PLAIN=1` or without a terminal: a plain menu or the status table).

| Entry | What it does |
|---|---|
| 1 Set up a new camera | first install from the stock firmware, nine resumable steps |
| 2 Update this camera | Akam to Akam over SSH, no card |
| 3 Camera options | LED and night, picture, video, audio, network (ONVIF, NTP, name, Wi-Fi shown read-only), security, advanced |
| 4 Back up / restore | back up the flash, restore the original |
| 5 Find cameras on my network | the camera chooser |
| 6 Settings | backup folder, state folder (shown), SD helper mode (shown), licences |
| 7 Check this computer | `doctor` |

Entries 1 to 4 open the camera chooser first when no camera is selected. The header shows the selected camera, its
address and its firmware.

### Camera chooser

Lists the cameras it knows and finds. Sources, cheapest first: the cameras you set up before (shown at once, probed in
parallel), a name (DNS, then `<name>.local`), ONVIF WS-Discovery, and a scan of your local /24 on the four camera ports
only (22, 24, 554, 8091). The two searches that send packets each ask first (default No) and show the range and the
ports. A camera that answers on 8091 is identified read-only (`GET fw.version`, `GET sys.hostname`). A stock camera
is only found when it already runs the toolkit card. VPN, point-to-point and virtual adapters are not scanned. If a
remembered camera got a new address and presents the same SSH host key, one yes/no (default No) re-pins it under
the new address; a key that differs is a hard error. Names a camera reports are cleaned before they are shown.

### Camera options

One screen per group, all values read in one go. Edits are made locally, validated like the command line, shown as
"Pending", and sent only on `apply`. Every option says whether it is saved on the camera and when it applies (now,
at the next video restart, at the next reboot). Toggles that open ports (`onvif enable`, `debug on`) or remove
protection (`rtsp-auth disable`) keep their yes/no, default No. The only camera process this program ever stops is the
video service, behind one yes/no (default No) after a video change ("the stream stops for a few seconds"). An option
the camera image does not know is shown as "not available on this image (update first)". The same settings exist as
commands: `led`, `picture`, `video`, `audio` (including `mic on|off`), `ntp`, `onvif`, `debug`, `rtsp-auth`,
`hostname`. Wi-Fi is displayed only; changing it needs the card route.

### Backup location

Asked the first time a backup is needed, and in Settings. The default is
`<Documents>/Akam backups/<camera>/<date>[-N]` (Linux: the XDG Documents folder, else `~/Documents`, else the home
folder; macOS `~/Documents`; Windows the Documents folder). Contents are the same as always: `mtdblock0.bin` to
`mtdblock7.bin`, `SHA256SUMS`, `README.txt`, `camera_info.txt`. The folder name is the camera name you chose, never a
name the camera reported. **The backup contains your Wi-Fi password in clear text: keep it private and off cloud
folders.** If Documents lives in OneDrive or iCloud Drive (or a path with Dropbox or similar), the suggestion falls back to
`~/Akam backups` (`%USERPROFILE%\Akam backups` on Windows) and says why. Folders and files are private (Linux and macOS
0700 and 0600; Windows a protected ACL for you, SYSTEM and Administrators; failure to set it is a visible warning).
A medium that cannot hold permissions (FAT, exFAT, a network share) shows "Private: no" and asks one yes/no (default
No). The folder must be new or empty, have 64 MiB plus the dump size free, and not be inside the repository or on the
card being written. Old `~/akam-backup-*` folders stay valid, `--backup-dir DIR` still works.

### Backup routes

* **Akam camera, over SSH (no card):** `backup` streams `/dev/mtdblockN` twice per partition over the SSH session,
  compares size and hash between the two reads, and writes the folder with `SHA256SUMS`, `camera_info.txt` and a marker
  `kind=akam`. It only reads. It saves what is on the camera now: **it is not a way back to the stock firmware**, and it is
  never used to restore stock. Only a backup made from the stock camera is a stock backup.
* **Stock camera, telnet and the toolkit card:** the P3 route (double read on the camera, `nc` transfer, hash checks).

### Update and first install

* **Update (Akam to Akam)**: kit downloaded and verified; image (built from your backup, or `--image FILE`); **backup
  gate**: a recorded backup folder must exist and its `SHA256SUMS` is re-hashed now; precheck on the camera (writes
  nothing); the red box with the image md5 and one yes/no (default No); the audited `flash.Flash`; then "unplug and
  replug USB-C by hand". A camera with no recorded stock backup is offered one first. After a failure that follows the
  start the message says **DO NOT power-cycle**. Ctrl-C or closing the window does not stop the camera-side worker.
* **First install** (nine steps, the same keys as `akam status`: doctor, get-kit, prepare-backup-sd, find-camera, backup,
  image, prepare-flash-sd, flash, verify): each card erase is a red box naming the device, size and model; the flash
  is a red box naming the camera, route and image md5. **Every flash is gated on a verified stock backup.**

### Licences and doctor

`akam licenses` prints the notices of the software built into the binary (the same text as `THIRD_PARTY_NOTICES.md`;
a binary downloaded alone still carries them). `akam doctor` is OS-aware: it shows the Linux distribution, the macOS or
Windows version, what works here, what is optional (`ffmpeg` or `arecord` for the microphone; `unsquashfs` and
`mksquashfs` only for the interim image rule below) and, on Windows, whether the console supports colours. It changes
nothing.

### Writing the SD card, per OS

The guards are the same everywhere (removable or SD/MMC card only, 128 MiB to 128 GiB, never a disk that holds the
system, a mounted repository or home, one red box, default No). Writing runs in a small helper that does only
`list`, `prepare`, `verify` and `wipe`, with the request on stdin (or a private channel), never in an argument.

| OS | How the helper gets its rights | Notes |
|---|---|---|
| Linux | `sudo` in a terminal or `pkexec` with a graphical session (`AKAM_SD_ELEVATE=sudo` or `pkexec`) | unchanged |
| Windows | one UAC prompt (the publisher shows as "Unknown publisher" while the binary is unsigned); the helper talks to the program over a private named pipe. Already running as Administrator: no prompt | close Explorer windows on the card first; BitLocker and write-protected cards are refused with a message |
| macOS | `sudo` in a terminal; an administrator dialog through `osascript` only when you opt in with `AKAM_SD_ELEVATE=osascript` | the internal disk and disk images are refused; a system volume that cannot be mapped fails closed |

Declining the prompt writes nothing and says so. The `wipe-sd` step erases a card that holds your Wi-Fi password
when you are done.

### Interim image rule (Windows and macOS)

The native squashfs builder is not available yet (reader merged; writer and assembly pending; the hardware gate of
spec 11, section 4.8, must pass before any user flashes with it). Until then **on Windows and macOS you import an
image built on Linux or in WSL**: build it with `./akam image` there, copy the 5 242 880-byte file, and pass
`--image FILE`. The program checks the size and records the md5, which the red box shows before the flash. On Linux
the default stays the bash `image` step.

### Unsigned binaries

The binaries are not signed yet. Windows SmartScreen ("Windows protected your PC"): More info, Run anyway; or
`Unblock-File .\akam-windows-amd64.exe` in PowerShell. macOS Gatekeeper: download with `curl -L -o` (no quarantine),
or `xattr -d com.apple.quarantine ./akam-darwin-arm64`, or Control-click Open once, or System Settings, Privacy &
Security, Allow Anyway; do not repack the arm64 binary. A raw Unix download needs `chmod +x`. Always compare the
SHA256 with `SHA256SUMS` first. Details: [docs/assistant_install.md](../../docs/assistant_install.md).

### Safety rules (phase 11, on top of the list further down)

* Every yes/no defaults to No and refuses without a terminal; red boxes name exactly what will be erased or written.
* The program never reboots or power-cycles a camera. After a flash you do it by hand.
* A backup gates every flash: the folder's `SHA256SUMS` is re-verified right before.
* An SSH backup of an Akam camera is never used to restore the stock firmware.
* Nothing is sent to your network (scan, discovery) before a yes.

## Build

Release builds use the official Go of `GO_VERSION` in `tools/build/deps.lock` (1.27.4; `go.mod` keeps `go 1.26.0`
as the language minimum). `bash tools/build/bootstrap.sh` downloads that archive (sha256-checked) into
`tools/.toolchain/go-<version>-<os>-<arch>/` (project-local, never system-wide); `build.sh` and the tests prefer it,
else use the `go` of PATH (dev and test actions then only warn; `all`, `flat`, `repro` and the release script refuse it
unless `AKAM_ALLOW_ANY_GO=1`). The modules are not stored in git: bootstrap also runs `go mod download` (checked against
`go.sum`); `build.sh` and the tests then work offline. Nothing is written
outside the repository: the module and build caches live in `tools/.toolchain/go/` (git-ignored).

```bash
tools/assistant/build.sh          # this computer  -> tools/assistant/bin/akam
tools/assistant/build.sh all      # the six targets -> tools/assistant/bin/<os>-<arch>/akam[.exe]
tools/assistant/build.sh test     # go vet and go test ./...
tools/assistant/build.sh repro    # two builds, same sha256
VERSION=1.0.0 tools/assistant/build.sh   # version shown by `akam version`
```

`make -C tools/assistant host|all|test` calls the same script. The build is static (`CGO_ENABLED=0`), uses
`-trimpath -buildvcs=false -ldflags "-s -w"`, and checks the 12 MiB size budget of the spec (with P3: 9.6 MiB for
amd64 and 8.9 MiB for arm64).

## Use

The commands and options have the names and the texts of `./akam help` (`akam help` prints the same text,
plus the three commands that only exist here). From a source checkout, the bash script can hand over to the
binary for a side-by-side comparison:

```bash
AKAM_USE_BIN=1 ./akam led            # runs tools/assistant/bin/akam led (or $AKAM_BIN)
tools/assistant/bin/akam --ip 192.168.1.50 audio volume 30
```

Both programs keep their progress in the same folder (`~/.local/state/akam`, `AKAM_STATE_DIR`,
`--state-dir`) with the same file format, so you can use one after the other. **Do not run both at once**
(the binary takes a lock; the script does not). The first time the binary opens a folder that already holds a
state file, it keeps one untouched copy as `state.bak-before-binary`.

| Command | What it does in this phase |
|---|---|
| `help`, `version`, `self-test` | the text, the version, offline checks (G.711, Digest, state file, the safety rule below) |
| `status`, `cameras`, `rename-camera NEW`, `register` | progress table, camera list, rename, and record a camera that already runs Akam (`--ip`, optional `--backup-dir`) |
| `find-camera` | asks the address (or a name: DNS, then mDNS `<name>.local`), probes the four ports in parallel and tells stock + toolkit from Akam. With no address typed, two opt-in searches, each behind its own yes/no (default No): ONVIF discovery (one UDP packet to 239.255.255.250:3702) and a scan of your local /24 (only the four camera ports) |
| `led`, `audio`, `picture`, `debug`, `onvif` | the same settings, checks and messages as the bash commands; control-port lines go through an SSH forward to the camera's own loopback |
| `rtsp-auth [set\|disable\|status]` | the password is typed twice without echo and sent on stdin to `akam_rtsp_auth.sh set-creds` |
| `hostname [NAME]` | `akam_hostname.sh set NAME` (a note only on an older image) |
| `talk [SECONDS\|test]`, `say FILE` | RTSP + Digest + ONVIF audio backchannel + G.711 in Go; `talk test` and `say file.wav` need no other program (any rate or channel count; 8/16/24/32-bit PCM or float). The microphone and files that are not WAV need `arecord` or `ffmpeg` (optional, found automatically, nothing else uses them) |
| `forget-host-key IP` | forget a camera's pinned SSH host key (asks first) |
| `doctor` | what this program found on your computer (optional tools included) |
| menu (`akam` alone) | on a terminal, a full-screen menu (arrows or `j`/`k`, Enter, `?`, `q`); with `--plain`, `AKAM_PLAIN=1` or without a terminal, the status table |

| `get-kit` | downloads `akam-kit-X.tar.xz`, `SHA256SUMS`, `SHA256SUMS.sig` (HTTPS only, TLS 1.2+, a redirect to http is refused, 3 tries), checks the sha256, the OpenSSH signature (SSHSIG, principal and namespace `akam-release`, keys of `tools/release/allowed_signers` built in; there is no environment override in a release binary: `AKAM_ALLOWED_SIGNERS`, `AKAM_KIT_URL_BASE`, `AKAM_FLASH_TOOL`, `AKAM_FLASH_SCRIPTS_DIR`, `AKAM_PORT_SSH2`, `AKAM_REFLASH_*` and `AKAM_FORCE_TTY` are test hooks, read only by a build with `-tags akamtest`; downloads are capped at 256 MiB and 1 MiB for the checksum files, the archive at 1 GiB decompressed / 200 MiB of content / 10000 entries), the archive layout and the kit's MANIFEST exactly like `verify_kit.sh`, and extracts it to `<state dir>/kit`. `--kit FILE`, `--version`, `--expect-sha256`, `--allow-unsigned` keep their meaning. Today's `allowed_signers` holds no key, so a signed kit is refused; an unsigned one needs the yes/no "Unsigned kit" (default No, refused without a terminal) or `--allow-unsigned`. Nothing from a kit is run. The `.tar.xz` is decoded in process (`ulikunitz/xz`, no `xz` program needed): before any byte is decoded the stream structure is checked (exactly one stream with one LZMA2 block, dictionary at most 64 MiB, declared size at most 1 GiB) |
| `update [--image FILE]` | flashes a new image over SSH, no card: precheck (writes nothing), the red box and one yes/no (default No), upload to the camera's RAM, sha256 on both sides, md5 by the camera script, a second Dropbear from RAM on port 2222, detached flash, polled log, independent read-back md5 of `/dev/mtd7`. The camera-side scripts are those of `tools/flash_scripts` (or the kit's `scripts/flash`), unchanged. It never reboots: power-cycle by hand |
| `restore --backup-dir DIR` | the same path with `mtdblock7.bin` of your backup (SHA256SUMS checked); over SSH when Akam runs (asked), else through the maintenance telnet and the card like `flash` |
| `verify` | waits for SSH, runs `akam_hwcheck.sh`, checks the ports (22, 554, 8091; 24 only with the card; 80 only when ONVIF is on) and the Wi-Fi gate, asks you to power-cycle without the card by hand, repeats the checks, then the optional password, ONVIF and camera-name offers |
| `backup` | over the maintenance telnet (port 24, toolkit card): `/proc/mtd` must be the supported 8-partition table, kernel and free space checked, the 8 partitions are `dd`-ed twice on the card, 16 exact sizes, both runs hashed on the camera and compared (CONFIG alone may differ: read again twice), dump1 copied to `~/akam-backup-<name>-<date>/` over `nc` with size + hash checks (3 tries, no partial file left), `SHA256SUMS`, `README.txt`, `camera_info.txt` as bash writes them, hardware verdict (yes/no in a red box, default No, unless validated), second-copy question. `--backup-dir DIR` accepts a folder made by hand |
| `prepare-backup-sd`, `prepare-flash-sd`, `wipe-sd` | see "The SD card" below. One red box (device path, size, model) with a yes/no default No, refused without a terminal |
| `flash` | stock or maintenance telnet with the flash card on `/mnt`: layout gate, exactly one image on the card with the right md5 (pushed over `nc` only after a yes), `flash_tool` and scripts checked or pushed, Wi-Fi gate (`preflight_check.sh --ensure-wifi` on the camera), the camera script's md5 line compared, the red box, and only after the explicit yes the `YES` to the camera's own prompt; then the read-back `dd if=/dev/mtd7 | md5sum` decides. SSH route when Akam runs and you say yes. No reboot is ever sent |

Still in the bash wizard: `image` and `build` (they need a squashfs writer; see the interim image rule). `--manual-telnet` keeps its meaning;
after two failed commands the session switches to manual mode on its own.

### The telnet client

`internal/camera/telnet` is a port of `camsh.sh` and `transfer.sh`. The PC only dials out. One command per send
(at most 350 bytes, one line), wrapped as `echo "__AKAM_BEG_<nonce>_<n>_"x; CMD; echo "__AKAM_DONE_<nonce>_<n>:"$?"__"`
so that the echo of the typed line never matches a marker; everything before the BEG marker is dropped; CR and
terminal escapes (BusyBox `ls` colours, `ESC[6n`) are stripped; progress bars of `flash_tool` feed the progress line;
the inactivity timeout is 30 s. RFC 854 negotiation: by default every request is dropped silently and never
answered, exactly like the bash wizard (validated on hardware). The refuse-everything-except-echo/SGA behaviour
(WONT/DONT, each answer once) exists only behind the hidden `--telnet-negotiate` option (or `AKAM_TELNET_NEGOTIATE=1`
in a test build) until it has been seen to work on a real camera. The marker nonce comes from `crypto/rand`, a line
is kept up to 64 KiB, and a transfer from the camera is cut off at the announced size. Manual mode
shows the exact command and takes the pasted output: the program never invents an output, and `YES` is only sent
through `sendYES`, which needs the `flash.Confirmation` of an explicit yes for that image's md5 (tested: any other
answer makes the script receive `NO`, a missing terminal sends nothing).

### The SD card

Enumeration needs no privilege: `/sys/class/block`, `/proc/self/mountinfo`, `/proc/swaps`. Eligible: a whole `sdX`
disk that is removable and USB-attached, or an `mmcblkN` of type SD; 128 MiB to 128 GiB; not carrying `/`, `/boot`,
`/usr`, `/var`, `/etc`, `/home`, `/opt`, `/srv`, `/nix` or swap; not a member of a stack (holders: LUKS, LVM, RAID,
device-mapper); the repository, `$HOME`, the working and the temp directories must not live on it; symlinks are
resolved first and a link is refused by the check itself. The identity (size, model, serial, wwn, vendor, major:minor)
travels as a token.

Writing goes through `akam sd-helper`, re-executed with `sudo` or `pkexec` (`AKAM_SD_ELEVATE=sudo|pkexec`; auto: pkexec
with a graphical session, else sudo) only when the user cannot already write the device, after the red box and a
second yes/no ("This step needs administrator rights to write the card. Continue? [y/N]"). The helper takes one JSON
request on stdin (so the Wi-Fi password is never in argv, nor in the environment: the helper starts with a minimal
one), answers one JSON document and exits. Closed operation set: `list`, `prepare`, `verify`, `wipe`. It runs the
same guards again, compares the identity token, unmounts, re-checks, opens `O_RDWR|O_EXCL`, and refuses anything that
is not a block device. The content to write travels in the request (the helper reads no file). `prepare` writes the
MBR (one active FAT32-LBA partition at 1 MiB), a FAT32 volume (label `AKAMSD`, 2 FATs, long names, cluster size by
card size), the files, syncs, re-reads the card with the built-in FAT32 reader and compares every file and the checks
of `sd_card.sh verify`. `wipe` zero-fills exactly the device size and spot-checks both ends. Unlike bash, the first-boot
card and the flash files are written in one pass (no second write on a mounted card). A release binary has no
image-file mode: the one that lets the helper write a regular file exists only under `-tags akamtest`.
The toolkit is the pinned `tasarren/lsc-tuya-toolkit` commit: a checkout in `AKAM_TOOLKIT_DIR` (default
`~/akam-sdprep/toolkit`), or, if missing, the GitHub tarball of that commit over HTTPS; either way the C-locale tree
hash must equal the pin of `sd_card.sh` (a test compares the Go card tree byte for byte with the one `sd_card.sh
--dry-run --stage-dir` builds, when the toolkit is present). Not done: LUKS/RAID signature probing of an inactive
partition (only active holders are seen). The Windows and macOS layers are described in "Phase 11" above.

### The flash path (audited)

* One function writes the flash: `flash.Flash` in `internal/flash`, called from `flashSSH` only (a test counts the
  callers and checks that the start command and the confirmation flag appear nowhere else).
* It needs a `flash.Confirmation`, which only `flash.NewConfirmation` can make, from an explicit yes (default No: Enter,
  Esc, `q` and end of input are No; refused without a terminal) for that image's md5. `--dry-run` never reaches it.
* The image must be exactly 5242880 bytes; its md5 is checked here, its sha256 after the upload, its md5 by the camera
  script and by the detached worker, then two read-backs (the worker's and an independent one) must equal it.
* The set of commands sent is closed and tested; no `reboot`, `poweroff`, `kill` or watchdog action ever appears. The P1
  guard of `internal/cli/remote.go` still refuses `flash_tool`, mtd writes and the reflash script on every other path.
* After a failure that follows the start the message says **DO NOT power-cycle**; after success it says to unplug and
  replug USB-C by hand.

### Image building (spike result)

The squashfs spike (specs/09, section 10.2) found no mature MIT/BSD pure-Go writer, so `image` stays in the bash
wizard (`mksquashfs`/`unsquashfs`, reported by `doctor`). `update` takes `--image FILE` or the image the bash step recorded.

### SSH and host keys

Login is `root` with the key `~/.ssh/akam` (or `--identity FILE`; with `--pubkey`/the state key the private key
is the file without `.pub`), the key the bash wizard creates and puts on the flash card. A key with a
passphrase is signed through `ssh-agent` or asks the passphrase without echo. Only `ssh-ed25519` host keys
are accepted (what `akam_ssh.sh` generates). The camera's key is pinned in `<state dir>/known_hosts` under the
camera's address, so one pin also covers a second Dropbear on port 2222. `~/.ssh/known_hosts` is never written,
and `~/.ssh/config` is ignored.

* First contact: the SHA256 fingerprint is shown and one yes/no is asked (default No). Without a terminal it
  refuses; pass `--trust-new-host-key SHA256:...` to accept that exact key. If the same key is already in your
  `~/.ssh/known_hosts` the question says so (the bash wizard recorded it there).
* A key that **changed** is a hard error, `--trust-new-host-key` included. Only `akam forget-host-key IP`
  (it asks, and logs it) clears a pin.

### Safety rules (tested)

* Every yes/no defaults to No (Enter, Esc, `q`, end of input) and refuses without a terminal; `debug on`,
  `onvif enable` and `rtsp-auth disable` ask exactly as in the bash wizard, the pure reads and the small
  reversible settings (`led`, `audio`, `picture`, `hostname`, `rtsp-auth set`) do not, again as in bash.
* All camera command lines come from `internal/cli/remote.go`, and the one function that sends them refuses
  `reboot`, `poweroff`, `halt`, `init`, `kill`, anything about the watchdog or a flash write. A test checks the
  whole set and the guard itself.
* Passwords are read without echo (or from `AKAM_RTSP_PASSWORD` / `--password-file`, removed from the
  environment before any other process starts), never in an argument, the state file or the log; the log and
  the state refuse registered secrets. Tests grep the state, the log, the output and every command the fake
  camera received.
* A capture or conversion process (`arecord`, `ffmpeg`) runs in its own process group, is stopped on every
  way out (end, error, Ctrl-C, TERM, HUP) and, on Linux, is killed by the kernel if this program dies.

## Tests

`go test ./...` (or `tests/assistant_test.sh`, which `tools/ci/local_check.sh` runs; it is skipped with a
message when Go is missing) covers every P1 command end to end without a camera, a microphone, a speaker or
ffmpeg:

* `internal/testfake`: the fake camera. An in-process SSH server that runs the **real helper scripts**
  of `tools/firmware_patch_templates/` (`akam_rtsp_auth.sh`, `akam_hostname.sh`, `akam_debug.sh`) in a
  temporary root, a control port 8091 reached through `direct-tcpip`, a second Dropbear and a fake flash (a byte slice plays
  `/dev/mtd7`; the behaviour of `akam_reflash_ssh.sh` is emulated, the real script is tested by
  `tests/reflash_ssh_test.sh`), and an RTSP server with Digest login
  against the credentials those scripts wrote and the ONVIF backchannel. `akam_onvif.sh` is emulated until it is
  part of this tree (it is on the ONVIF branch).
* the P3 tests: `internal/testfake/stock.go` is a fake stock camera (telnet server over a temporary tree, BusyBox-like
  echo and IAC bytes, mtd "devices" as files, a fake `install_with_flash_tool.sh`; fault injection: dropped
  commands, closed session, corrupted or truncated `nc` transfers, unreliable reads, bad write) used by the backup,
  flash and restore tests; the SD tests run on a regular file (`internal/sdcard` guard matrix on a fake `/sys`,
  FAT32 validated by the built-in reader and by `fsck.vfat -n` when installed, helper refusals, toolkit download over a
  test HTTPS server). Tests that need the image-file mode or test hooks run with `-tags akamtest`
  (`tools/assistant/build.sh test` and `tests/assistant_test.sh` do);
* the P2 tests: kit refusal cases, SSHSIG interoperability with `ssh-keygen -Y` in both directions, verdict parity with
  `tools/release/verify_kit.sh` on 25 kits, and the whole update, restore and verify flows on the fake camera;
* golden files (`testdata/golden/*.golden`) made from the **bash** wizard by `testdata/gen_golden.sh`; the Go
  output must equal them byte for byte (`testdata/scenarios.txt` lists the scenarios);
* a state round trip with the real `./akam` in both directions;
* the bubbletea models driven by scripted keys and compared with `internal/ui/tui/testdata/*.golden`
  (`go test ./internal/ui/tui -update` rewrites them).

## Layout

```
cmd/akam/                 main: picks the UI, calls cli.Run
internal/cli/             dispatcher, options, one file per group of commands, steps and status
internal/ui/              the UI interface and the plain implementation; ui/tui = bubbletea
internal/state/           state files, cameras, log with redaction, lock
internal/camera/ssh/      SSH client, keys, host-key pinning
internal/camera/ctl/      control port 8091 client
internal/camera/telnet/   maintenance telnet client, manual mode, nc transfers with hashing
internal/sdcard/          card enumeration and guards, MBR + FAT32 writer and reader, payload, helper, toolkit
internal/assets/          embedded camera-side scripts (go generate) and the supported-hardware list
internal/kit/             kit download, SSHSIG check, archive and MANIFEST checks (port of verify_kit.sh); kit/kittest builds test kits
internal/flash/           the one audited function that writes the camera flash (port of reflash_over_ssh.sh)
internal/discover/        port probe, subnet list and scan, WS-Discovery, mDNS
internal/rtsp/            RTSP + Digest, ONVIF backchannel talk, G.711, WAV
internal/audio/           optional capture tools (external processes)
internal/testfake/        fake camera for the tests
```

## Known gaps against the bash wizard (P1)

* `find-camera` does not need `prepare-backup-sd` done first when the camera already runs Akam (ssh, rtsp and
  control ports open, telnet closed): it proceeds. Any other camera still gets the bash rule ("needs these
  steps first"). The bash wizard keeps the strict rule on every camera.
* `say` decodes WAV itself and uses `ffmpeg` only for other formats (the bash script always used `ffmpeg` when
  present), so its rehearsal line differs; `pw-record`/`parec` capture backends are not used yet.
* Messages say `./akam` (as the bash wizard does) even when the binary is run under another name.
* `onvif` follows the command of the ONVIF branch (`akam_onvif.sh`), which is not merged into `dev` yet.
* P2: the `update`/`restore` flow was tested against an emulation of the camera scripts, not the scripts themselves
  (they have their own tests in `tests/reflash_ssh_test.sh`); `get-kit` on a
  folder (`--kit DIR`) is accepted only as an unsigned kit.
* P3: not seen on a real camera or a real card. Real hardware is needed for: the telnet negotiation answers (bash
  stayed silent), the BusyBox prompt and echo details of the real shell, `nc` timing, the card booting the camera
  into telnet 24 (and the camera reading a card written by this FAT32 writer), `sudo`/`pkexec` elevation, a real
  USB reader and `/sys` layout, and a first install from stock end to end.
* `akam telnet [IP]` (interactive passthrough) is not implemented; `prepare-*-sd` write the card in one pass (bash
  prepared, then added the flash files).
