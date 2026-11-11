# Akam - Specification: the assistant on every platform, guided and automated (phase 11)

Status: proposal for review, written for the coordinator. Language of the tool: English (spec 02, DA2).
Nothing here was run against a camera or a real card. File:line references are against `dev` at bd399c6
(`tools/assistant` is the Go module; paths below are relative to the repository root unless they start
with `internal/` or `cmd/`, which are relative to `tools/assistant/`). Nothing was downloaded to write this
spec: the one third-party module it adds (`github.com/ulikunitz/xz` v0.5.17, BSD-3-Clause) was read from the
project-local module cache that the user already authorised.

Builds on specs/09 (the single binary, decisions 10.1 and 10.2). Where this spec changes a decision of 09 it
says so (section 1.3).

---

## 0. Summary

What the user asked for (R1 to R8) and the shape of the answer:

1. **One static binary per platform, six targets**: linux, windows, darwin x amd64, arm64. `CGO_ENABLED=0`.
   Good news measured on this repository today: **all six targets already compile** with the current code
   (`go build` for each, with the project flags; sizes in 2.6). The gaps are behavioural, not build errors.
2. **xz in process** with `github.com/ulikunitz/xz` v0.5.17. One finding changes the design: in that library
   `ReaderConfig.DictCap` is a *floor*, not a cap (`lzmafilter.go:75-76`: a larger dictionary size in the
   stream header wins). A stream header can therefore ask for a multi-GiB dictionary. The cap must be
   enforced by reading the stream metadata before decoding (4.3).
3. **The assistant becomes a guided program**, not a list of commands: a home screen, a camera chooser
   backed by discovery, a data-driven option editor, a first-install flow and an update flow, a backup
   location chooser (section 3).
4. **SD card writing on Windows and macOS** with the same guard semantics as Linux, designed so that the
   *decision* logic is one pure function fed with plain facts, testable on Linux for all three OSes (4.4).
5. **The hard blocker nobody listed: the `image` step.** First install from a stock camera needs an image
   built from the user's own APP dump with `unsquashfs` and `mksquashfs` (`tools/image/assemble_from_kit.sh:48`,
   `src/ak_rtsp/build_firmware.sh:119,609`), plus shell patches. Those tools do not exist on Windows or macOS.
   Spec 09 section 10.2 deferred a native writer. R1 and R3(d) cannot be met on Windows/macOS without it.
   This spec plans it as an explicit, gated track (4.8, T11.12 to T11.16) with an interim (`--image FILE`).
6. **Windows has three more real problems found by reading the code**: `os.Symlink` in kit extraction needs a
   privilege Windows users do not have (`internal/kit/verify.go:445,794`); `net.Interfaces` filtering and
   multicast discovery are Linux-shaped (`internal/discover/discover.go:132`, `wsd.go:77`); raw ANSI output is
   written without enabling the console's VT mode (`internal/ui/plain.go:56`, `internal/cli/app.go:253`).
7. **Sixteen dev tasks**, at most two in parallel, no file overlap inside a pair (section 6), then a manual
   hardware plan per OS (section 7).

Decisions the coordinator must take are collected in section 9, each with a recommended answer.

---

## 1. Goals, non-goals, changes to earlier decisions

### 1.1 Goals

- G1 (R1) One standalone binary per platform: `linux/amd64`, `linux/arm64`, `windows/amd64`, `windows/arm64`,
  `darwin/amd64`, `darwin/arm64`. No runtime dependency for the core flows: no `xz`, `ssh`, `ssh-keygen`,
  `bash`, `ffmpeg`, `sudo`-less assumptions. OS-provided tools that are part of the operating system itself
  (`diskutil` on macOS, `pkexec`/`sudo` on Linux, the UAC prompt on Windows) are allowed and named in 4.4.
- G2 (R2) xz decoding in process, size-capped, memory-capped, zip-bomb safe.
- G3 (R3) Guide and automate: choose a backup location, choose a camera (discovery, remembered cameras),
  view/edit/enable/disable camera options, update an Akam camera over SSH, first flash of a stock camera
  end to end. Backup first, mandatory and verified. One explicit yes/no (default No) per destructive step, and
  the final confirmation bound to the image md5 (the existing `flash.Confirmation`, `internal/flash/flash.go:87-108`).
  **Never power-cycle or reboot automatically** (unchanged rule of spec 09, enforced by `internal/cli/remote.go:57`).
- G4 (R4) SD card preparation on Linux, Windows and macOS with the same guards and the same read-back
  verification.
- G5 (R5) Close the other platform gaps: state lock, audio decision, console, paths, permissions, line endings,
  Ctrl-C.
- G6 (R6) Size budgets per platform and reductions.
- G7 (R7) Cross-compile, reproducible builds, unit tests for every new piece of logic, a manual hardware plan.
- G8 (R8) Release naming, checksums, notices alongside, unsigned-binary instructions.

### 1.2 Non-goals

- A GUI or web UI. A second UI toolkit. Localisation. Auto-update (still none).
- Other camera models; the hardware gate `supported_hardware.txt` is unchanged.
- Changing what runs on the camera, **except** where an option has no camera-side command today. Only Wi-Fi
  editing is in that case (4.5): it stays out of this phase (question Q7).
- Re-implementing `./akam build` (Zig, make, SmolRTSP): stays bash.
- Retiring the bash wizard (spec 09 section 9.6 still applies).
- A pure-Go microphone capture on macOS (would need a new dependency or cgo: 4.9.6).

### 1.3 Changes to decisions of spec 09

| Spec 09 | Now | Why |
|---|---|---|
| 0.2 "ffmpeg stays optional, only for talk/say" | unchanged | still true; this spec only fixes the Windows device naming bug (2.5) |
| 3.7 / 10.1 "first Windows release is files-only SD" | **replaced**: full raw SD writing on Windows and macOS, with a files-only fallback kept as an escape hatch (4.4.9) | the user asked for automation on every OS (R4) |
| 10.2 "image stays in bash" | **kept as the default until a gate passes**, then replaced by a native builder (4.8) | R1 and R3(d) need it; interim `--image FILE` |
| 9 "P4 = macOS and Windows" | this phase is P4 and goes further (guided flows, options, discovery) | R3 |
| 7.1 release names `akam-X.Y.Z-<os>-<arch>` | `akam-<os>-<arch>[.exe]`, version in the release tag `assistant-vX.Y.Z` (8.1) | R8 |

---

## 2. Current state and gaps (verified against `dev`)

### 2.1 What already works for this phase

- Static build, `-trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."`, `CGO_ENABLED=0`:
  `tools/assistant/build.sh:36-38`. Two-build reproducibility check: `build.sh repro`.
- The control port 8091 is **readable by any LAN client without a password** (docs/akam_v1_install.md, section 4:
  `LIST`, `GET` work for every client; `SET` only from the camera). This makes LAN discovery with firmware
  version and hostname possible without SSH (4.6).
- `flash.Confirmation` is only creatable from an explicit yes bound to the image md5; one function writes the
  flash (`internal/flash/flash.go:87-108`, `internal/cli/flashcmds.go:198`).
- The FAT32 and MBR writers work on an interface, not on `*os.File`: `sdcard.Device` = `ReaderAt` + `WriterAt` +
  `Sync` (`internal/sdcard/fat32.go:21-25`). That is the seam for Windows and macOS.
- The privileged helper already takes one JSON request, re-validates every guard and compares an identity token
  (`internal/sdcard/helper.go`, `internal/sdcard/sysfs.go:313`).
- bubbletea already carries Windows console input code (`erikgeiser/coninput`, listed in THIRD_PARTY_NOTICES.md
  as "linked only in Windows builds") and enables VT output itself (`bubbletea@v1.3.10/tty_windows.go`).

### 2.2 Gaps by area

| # | Area | Finding | Where |
|---|---|---|---|
| 1 | xz | the kit is opened by running the external `xz` program; tests also need it | `internal/kit/verify.go:106-143` (`xzDecompress`, `procReader`), `:167-168` (default), `internal/kit/kittest/kittest.go:26-28,218`, `internal/cli/misccmds.go:54` (doctor line), `README.md` of the module (xz sentences) |
| 2 | xz limits | a 1 GiB stream cap and 200 MiB content cap exist; they bound output, not the decoder's dictionary memory | `internal/kit/verify.go:26-28,315,376-403` |
| 3 | Kit on Windows | extraction creates real symlinks (`overlay/bin/dropbear -> dropbearmulti`) with `os.Symlink`; Windows refuses without Developer Mode or admin. Directory-mode verification also compares file modes, which Windows does not have | `internal/kit/verify.go:365,445,513,672,789-794`, mode checks `:357,362` |
| 4 | Kit path safety | extraction joins a cleaned archive name with `filepath.Join` and trusts the name regex `^[A-Za-z0-9._+/-]*$`; that blocks `\` and `:` but not Windows device names (`NUL`, `COM1`) | `internal/kit/verify.go:301,334,436` (no `filepath.IsLocal`; `IsLocal` is used elsewhere: `cameracmds.go:193`, `payload.go:189`, `toolkit.go:92`) |
| 5 | State lock | no-op off unix | `internal/state/lock_other.go:1-12`; unix version `lock_unix.go` (`Flock`) |
| 6 | State directory | `~/.local/state/akam` on every OS | `internal/state/state.go:20-29` |
| 7 | Camera names | `NameValid` accepts `con`, `nul`, `aux`, `com1`...; the state layout uses the name as a directory (`<base>/cameras/<name>`) | `internal/state/cameras.go:20-31,42-48` |
| 8 | Backup folder | `~/akam-backup-<name>-<date>[-N]`, no choice, SHA256SUMS written 0644 | `internal/cli/telnetcmds.go:409-425`, `:432` |
| 9 | Console | raw `\033[` colours when stderr is a terminal; nothing enables VT processing on Windows for the plain UI | `internal/ui/plain.go:54-59`, `internal/cli/app.go:253` |
| 10 | Signals | `signal.Ignore(os.Interrupt, SIGHUP, SIGTERM)` while flashing; `Notify` in talk. They compile on Windows but only `os.Interrupt` and the console-close event (delivered as SIGTERM) ever arrive | `internal/cli/sdcmds.go:28-31`, `internal/cli/talkcmds.go:131` |
| 11 | SSH agent | only `net.Dial("unix", $SSH_AUTH_SOCK)`; Windows OpenSSH agent is the named pipe `\\.\pipe\openssh-ssh-agent` | `internal/camera/ssh/keys.go:114-141` |
| 12 | Key file mode | `0o600`, `0o700` passed to `os.WriteFile`/`MkdirAll`: ignored on Windows (inherited ACL applies) | `internal/camera/ssh/keys.go:67-70`, `client.go:305-308,344`, `internal/state/cameras.go:60-63,183-194` |
| 13 | SD enumeration | the whole guard is Linux: `/sys/class/block`, `/proc/self/mountinfo`, `/proc/swaps`, names `sdX`/`mmcblkN`, `removable`, `/usb` in the sysfs path | `internal/sdcard/sysfs.go:42-64,201-310` |
| 14 | SD OS calls | `osUnmount` returns "Linux only" elsewhere; `osRescan` is a no-op | `internal/sdcard/os_other.go:1-14`, `os_linux.go` |
| 15 | SD elevation | `pkexec`/`sudo` only; request on stdin | `internal/sdcard/backend.go:55-126`; entry `cmd/akam/main.go:34-36` |
| 16 | SD I/O alignment | the writers issue arbitrary-size `WriteAt` calls; Windows physical drives and macOS `/dev/rdiskN` need sector-aligned offset and length; the sector size is a constant 512 | `internal/sdcard/fat32.go:35,284-287,380` |
| 17 | SD read-back | Linux flushes the block cache with `BLKFLSBUF` before re-reading; nothing equivalent for Windows (file cache) | `internal/sdcard/os_linux.go:11-17` |
| 18 | Discovery | virtual interface filter is a list of Linux names (`docker`, `veth`, `virbr`...); WS-Discovery and mDNS send from `ListenPacket("udp4", ":0")` so the outgoing interface is the OS default | `internal/discover/discover.go:132-141`, `wsd.go:77,186` |
| 19 | Camera options | the Go assistant has no `video`, `ntp`, or `audio mic` commands although the bash wizard has them (`./akam help`: `video`, `ntp`; `audio.mic_enable` at `tools/wizard/lib/steps_camera.sh:1442`). `internal/cli/help.txt` lists none of them. No generic option editor | `internal/cli/help.txt`, `internal/cli/ctlcmds.go` |
| 20 | Video restart | the bash `video` command offers to restart the video service by `kill $(pidof ak_rtsp)` (`steps_camera.sh:1698-1703`); the Go guard refuses every `kill` (`internal/cli/remote.go:57`) | see Q8 |
| 21 | Backup over SSH | only the telnet route (stock camera with the toolkit card) or `--backup-dir DIR` exists; no `dd`-over-SSH backup for a camera that already runs Akam | `internal/cli/telnetcmds.go:586-600` |
| 22 | Image | `image` and `build` are "not yet" in the Go binary; the bash image path needs `unsquashfs`, `mksquashfs` and shell patches | `internal/cli/app.go:196-201`, `tools/image/assemble_from_kit.sh:48,67`, spec 09 10.2 |
| 23 | Audio | microphone and non-WAV files need `arecord`/`ffmpeg`. On Windows the ffmpeg command uses `-f dshow -i audio=default`, but dshow needs a real device name, so it fails on most machines | `internal/audio/capture.go:56-80` |
| 24 | Process control | `Setpgid`/`Pdeathsig` are Linux only; others just `Process.Kill` | `internal/audio/proc_linux.go`, `proc_other.go` |
| 25 | Doctor | OS detection knows `arch`, `debian`, `ubuntu`, `unknown` only; message "only Arch Linux and Debian/Ubuntu are supported (macOS is planned)" | `internal/cli/hostinfo.go:16-60`, `misccmds.go:62` |
| 26 | Line endings | `.gitattributes` is `* text=auto`: on Windows a checkout with autocrlf would turn the embedded camera scripts (`internal/assets/scripts/*.embed`) into CRLF; a test exists for drift against the sources, not for CRLF | `.gitattributes`, `internal/assets/assets_test.go` |
| 27 | Release | no release script for the assistant exists (spec 09 7.3 names `tools/release/build_assistant.sh`; `tools/release` holds only the kit scripts). The Go toolchain is not pinned (only a minimum `GO_VERSION=1.26.0` in `tools/build/deps.lock`). There is no CI: tests run locally through `tools/ci/local_check.sh` (tools/release/README.md: "There is no CI") | `tools/build/deps.lock`, `tools/release/` |
| 28 | Cross-compile test | `tests/assistant_test.sh` builds and checks only the host binary; `build.sh all` builds linux only | `tests/assistant_test.sh:51-58`, `tools/assistant/build.sh:59-64` |
| 29 | Release key | `internal/kit/allowed_signers` is a placeholder with no key, so a signed kit is refused | `internal/kit/allowed_signers` |

### 2.3 Measured: the six targets compile

Command (module cache read from the existing project-local cache, build cache in a scratch directory, no
network, `GOFLAGS=-mod=readonly CGO_ENABLED=0 GOPROXY=off`), flags of `build.sh` (`-trimpath -buildvcs=false -ldflags "-s -w"`):

| Target | Bytes | MiB |
|---|---|---|
| linux/amd64 | 10,051,744 | 9.59 |
| linux/arm64 | 9,306,272 | 8.88 |
| darwin/amd64 | 10,252,080 | 9.78 |
| darwin/arm64 | 9,535,106 | 9.09 |
| windows/amd64 | 10,409,472 | 9.93 |
| windows/arm64 | 9,498,624 | 9.06 |

Caveats: the host toolchain was `go1.27.3-X:nodwarf5`, not the `1.26.0` named in `go.mod`, so absolute sizes can
differ with the pinned toolchain; the linux/arm64, darwin/amd64 and windows/arm64 figures come from a first run
that hit a full disk after linking (the other three were rebuilt in a second, clean run and gave identical
sizes for the two targets built twice, so the first run's numbers are believed correct but should be
re-measured by T11.5). **The user's figure of 14.6 MiB for windows/amd64 is not reproduced**: here it is 9.93 MiB
with the project flags. Possible causes to check, in this order: a build without `-s -w`
(unstripped it is 11.1 MiB here), a different toolchain, a build with `-tags akamtest`, a different module
set. T11.5 must reproduce the figure with the pinned toolchain before any budget is raised.

### 2.4 Compile-clean is not behaviour-clean

Because the code compiles everywhere, nothing warns a maintainer that on Windows the lock does nothing, the SD
layer reports no card and refuses to unmount, and kit extraction fails. Section 7.1 therefore adds
`GOOS=<os> go vet` for every target and a set of tests that run on Linux but exercise the per-OS logic.

### 2.5 Facts about the camera side that the flows rely on (not changed here)

- Control port keys read by LAN clients: `fw.version`, `sys.hostname` (API v2), `net.ip`, `rtsp.auth`,
  `onvif.state`, `ctl.api` ...; `net.ssid` and `net.mac` read as `hidden` for other machines unless debug mode is
  on (docs/akam_v1_install.md section 4); over the SSH loopback they are readable.
- `INFO <key>` (API v2) returns type, min, max, rw and persist flags (specs/04, section 2.3, command table): the option catalog
  can check itself against a camera at run time.
- A stock camera exposes telnet 24 **only** when booted with the maintenance toolkit card; without the card it
  has no open port the assistant can use. LAN discovery of stock cameras therefore only finds cameras that
  already run the card (4.6).
- The backup dump contains the CONFIG partition, which holds the Wi-Fi password in clear text: the backup folder
  is a secret (3.2 and section 5).

---

## 3. UX flows

All screens exist in two renderings: the bubbletea full-screen UI and the plain line-based UI (`--plain`, no
terminal). Both implement the same `ui.UI` interface (`internal/ui/ui.go`), so every flow is written once. Texts
below are mockups, 78 columns. The existing rules stay: English, every command shown before it runs,
`--dry-run`, one yes/no per state-changing step with default No (Enter, Esc, `q`, end of input), destructive
steps in the red box (`UI.Danger`) that refuses without a terminal.

### 3.1 Home screen

```
 Akam 1.3.0                                         windows/amd64
 Camera: salon  192.0.2.21  Akam v1.2.0-3     (c) change camera

   1  Set up a new camera        first install from the stock firmware
   2  Update this camera         Akam to Akam, over SSH, no card
   3  Camera options             LED, picture, video, audio, network, security
   4  Back up / restore          dump the flash, restore the original
   5  Find cameras on my network
   6  Settings                   backup folder, state folder, licences
   7  Check this computer        what works here, what is missing
   q  Quit

 Up/Down or j/k, Enter, ? help, q quit
```

With no camera chosen the header says `Camera: none (press c)`. Items 1 to 4 start with the camera chooser (3.3)
when needed. The first run of the program goes through 3.2 first, once.

### 3.2 Backup location (R3a)

Asked the first time a backup is needed (and in Settings). The default is per OS, shown in full, with one
question and a way to change it:

```
 Where should the backup of the camera be kept?

 The backup is the only way back to the original firmware, and it contains
 your Wi-Fi password in clear text. Keep it private and off cloud folders.

   Suggested:  C:\Users\<you>\Documents\Akam backups\salon\20261111
   Free space: 41.2 GB     Private: yes (only your account)

   [Enter] use this folder   [c] choose another folder   [q] cancel
```

Rules (implemented in a pure package, T11.4):

- Root by OS: Linux `$XDG_DOCUMENTS_DIR` from `~/.config/user-dirs.dirs` if present, else `~/Documents` if it
  exists, else `$HOME`; macOS `~/Documents`; Windows the Documents known folder
  (`windows.KnownFolderPath(FOLDERID_Documents)` from `golang.org/x/sys`, already a dependency).
  Subfolders `Akam backups/<camera>/<YYYYMMDD>[-N]` (one folder per camera, then one per date; the `-N` suffix applies to the date folder). Contents unchanged from today (`mtdblock0.bin`..`mtdblock7.bin`,
  `SHA256SUMS`, `README.txt`, `camera_info.txt`), so `--backup-dir DIR`, `backupAccept` and the bash wizard still work.
  A user who already has `~/akam-backup-*` keeps them; the state key `backup_dir` records the absolute path.
- **Cloud-folder warning**: if the root is inside OneDrive (Windows: environment `OneDrive`, `OneDriveConsumer`,
  `OneDriveCommercial`; a Documents folder redirected into one of them), iCloud Drive (macOS:
  `~/Library/Mobile Documents/`; Documents synced when "Desktop & Documents" is on, detected by the
  `com~apple~CloudDocs` path), Dropbox or similar names in the path, the suggested folder is
  `%USERPROFILE%\Akam backups` / `~/Akam backups` instead and the screen says why.
- **Private permissions**: Linux/macOS directories `0700` (`Akam backups`, `<camera>` and the date folder, all made private before any secret is written; the date folder is made with a plain `mkdir`, never taken over if it exists), files `0600` (SHA256SUMS and the text files are
  currently `0644`: `telnetcmds.go:432`; make them `0600`). Windows: each of these directories gets a protected DACL
  (no inheritance) with full control for the current user, SYSTEM and Administrators
  (`SetNamedSecurityInfo` from `x/sys/windows`); failure to set it is a visible warning, not an error. A
  folder on FAT/exFAT, a network share or a medium that cannot hold permissions shows
  `Private: no (this medium has no permissions)` and asks one yes/no (default No).
- **Validation**: the folder must not exist or must be empty (existing rule); not inside the repository; not on the SD card the
  assistant is about to write (the SD guards already reject this); free space at least 64 MiB plus the dump size
  (the dump is 8 partitions, the same size as the flash); Windows: no reserved device names, no trailing dot or
  space, no characters `<>:"|?*`, long paths through the `\\?\` prefix; macOS/Windows folders are case-insensitive,
  so `-N` suffix collision checks compare case-folded names.
- The folder name uses the **camera name chosen by the user** (validated by `state.NameValid`), never the
  hostname that the camera reported (section 5.4).

### 3.3 Choose the camera (R3b)

```
 Cameras on your network                       scanned 192.0.2.0/24 in 4 s

     Name        Address        Kind          Firmware         Remembered
  1  salon       192.0.2.21     Akam          v1.2.0-3-gab12   yes: salon
  2  garage      192.0.2.34     Akam          v1.1.0           yes: garage (was .30)
  3  (not named) 192.0.2.57     Akam          v1.2.0-3-gab12   no
  4  -           192.0.2.64     Stock + card  -                no
  -  attic       (last seen 192.0.2.40, 3 days ago, not answering)

 [Enter] choose   [r] scan again   [t] type an address   [f] forget a camera   [q] back
```

Sources, cheapest first, each behind the existing opt-in rule (4.6): (1) cameras remembered in the state
(shown instantly, probed in parallel with the 1.5 s dial of `OpenPorts`); (2) the name as DNS then mDNS
`<name>.local`; (3) ONVIF WS-Discovery; (4) a TCP scan of the local /24 on the four camera ports, followed by a
read-only `GET fw.version` / `GET sys.hostname` on port 8091 of each camera that answers. The scan question
stays a yes/no with default No and shows the range and the ports first
(`Scan 192.0.2.0/24: 254 addresses, ports 22, 24, 554, 8091 only? [y/N]`). Choosing an entry records it: a
new camera asks for a short name (validated), a known one is updated (address, hostname, firmware, last seen).
If a remembered camera's address changed, the SSH host key pinned for the old address is compared with the
key presented by the new address; an equal key means the same camera, and one yes/no (default No) re-pins it
under the new address ("same camera, new address").

### 3.4 Camera options (R3c)

One screen per group, all values read in one go (a single SSH session and one batch of `GET` lines).

```
 Camera options - salon (192.0.2.21)                         Akam v1.2.0-3

 > LED and night       front LED: auto         IR LEDs: auto     night: auto*
   Picture             brightness 0  contrast 0  saturation 0  sharpness 0
   Video               bitrate 1536 kbit/s   15 fps   key frame every 2 s
   Audio               speaker 40 (on)   talk: on   microphone: on
   Network             ONVIF: off   NTP: off   name: salon   Wi-Fi: HomeNet (read-only)
   Security            video password: off
   Advanced            debug mode: off

 * not saved: back to the default at the next reboot

 [Enter] open   [r] reload   [q] back
```

```
 Video - salon

   bitrate     [ 1536 ]  500 to 3000 kbit/s        saved, applies at the next video restart
   frame rate  [   15 ]  10 or 15
   key frames  [    2 ]  1 to 4 seconds

   Pending: bitrate 1536 -> 2000
   [a] apply   [d] discard   [q] back
 ...
 Saved on the camera. It applies when the video service restarts.
 Restart the video service now? The stream stops for a few seconds and starts again by itself. [y/N]
```

Rules: values are edited locally and validated by the same validators as the command line; nothing is sent
until `apply`; every option shows whether it is saved on the camera and **when it applies** (now, at the next
video restart, at the next reboot); toggles that open ports (`onvif enable`, `debug on`) or remove protection
(`rtsp-auth disable`) keep their existing yes/no (default No). The video restart is the only option that kills
a camera process (Q8). Passwords are typed twice without echo (existing `rtsp-auth set` behaviour). A camera that
does not know a key (older image) answers `ERR`; the screen shows that option as `not available on this image
(update first)` instead of failing. The catalog and its mapping to existing camera commands are in 4.5.

### 3.5 Update an Akam camera (R3d, Akam to Akam)

```
 Update salon (192.0.2.21), Akam v1.1.0 -> v1.2.0

   [done]  1  Kit v1.2.0 downloaded and verified (signature, checksums)
   [done]  2  Image built from your backup   md5 9f2c...41aa  5242880 bytes
   [done]  3  Backup checked: C:\Users\<you>\Documents\Akam backups\salon\20261001  8 files, SHA256SUMS ok
   [todo]  4  Pre-checks on the camera (writes nothing)
   [todo]  5  Write the image   <- the only step that changes the camera
   [todo]  6  Verify, then YOU power-cycle the camera
```

Order and gates: kit (verified) -> image (native or `--image FILE`) -> **backup gate**: a recorded backup folder
must exist and its `SHA256SUMS` must verify *now* (re-hashed, not trusted from an earlier run). A camera that
runs Akam and has no recorded stock backup is offered a backup over SSH first (3.7) -> pre-check (writes
nothing) -> the red box with the image md5 and the single yes/no (default No) -> `flash.Flash` (unchanged
two-stage path, second Dropbear on 2222, read-back md5) -> the message "unplug and replug USB-C by hand". After
a failure that follows the start: **DO NOT power-cycle**. Ctrl-C and window close do not stop the camera-side
worker (it runs detached on the camera by design).

### 3.6 First install from a stock camera (R3d)

Same screen style, nine steps that mirror the existing state steps (`state.Steps`: doctor, get-kit,
prepare-backup-sd, find-camera, backup, image, prepare-flash-sd, flash, verify), run in order and resumable:

```
 Set up a new camera - first install

   1 Check this computer        done
   2 Get and verify the kit     done
   3 Choose the backup folder   C:\Users\<you>\Documents\Akam backups\salon\20261111
   4 Prepare the backup card    microSD 32 GB "SanDisk SD"  (will be erased)   <- asks
   5 Find the camera            put the card in the camera, power it on, wait 2 minutes
   6 Back up the camera         8 partitions read twice, compared, copied here, verified
   7 Build the Akam image       from your own backup (nothing is downloaded from the vendor)
   8 Prepare the flash card     microSD (will be erased); Wi-Fi name and password asked here
   9 Write the camera flash     the red box, the md5, one yes/no
  10 Check and power-cycle      you unplug and replug the camera yourself
```

Each card erase is a red box naming the device, size and model, then (when needed) a second yes/no "This step
needs administrator rights to write the card. Continue? [y/N]". The red box of step 9 states the camera, the
route, the image md5 and the warning that a power cut during the write can brick the camera (existing text,
`flashcmds.go:80-97`). **Step 7 is the dependency discussed in 4.8.** On Windows and macOS, until the native
builder passes its gate, step 7 asks for an image file built elsewhere (`--image FILE`: size must be exactly
5242880 bytes; the red box in step 9 shows its md5) and says so plainly.

### 3.7 Backup and restore

- Stock camera with the card: the existing telnet route (`backup`).
- Akam camera, no card: **new** `backup --ssh` route (T11.11): `dd if=/dev/mtdblockN` streamed over the SSH
  session twice per partition, size and hash compared between the two reads, written to the chosen folder with
  `SHA256SUMS`, `camera_info.txt` and a `kind=akam` marker. It only reads. It cannot recreate the *stock*
  firmware that an Akam camera no longer has: the screen says so ("this saves what is on the camera now").
- Restore: existing `restore` (SSH when Akam runs, else telnet and the card), same red box, same md5 binding.

### 3.8 Settings and doctor

Settings: backup folder (3.2), state folder (read-only display of the path in use), "Licences" (prints the
embedded `THIRD_PARTY_NOTICES.md`, new command `akam licenses`, T11.13), SD helper mode for Linux
(`AKAM_SD_ELEVATE` shown, not edited). `doctor` becomes OS-aware: Linux distro (existing), macOS version,
Windows version; it lists what works here (`xz` is no longer listed), what is optional (`ffmpeg` for the
microphone, `unsquashfs`/`mksquashfs` only until the native builder lands), and for Windows whether the console
supports colours.

---

## 4. Architecture and platform abstraction

### 4.1 Package map and build tags

Existing packages stay. New or reshaped ones (new files carry the OS in the name so that `GOOS` selects them
without `//go:build` lines where the Go filename suffix rules suffice; explicit tags are noted):

```
internal/platform/           NEW   per-OS small facts: DefaultStateDir, DocumentsDir, PrivateDir(path), IsElevated,
                                    EnableVT, SignalsToIgnore, TerminalCheck. platform_{linux,darwin,windows}.go
internal/fsutil/             NEW   AtomicWrite (rename with retries on Windows), PrivateMkdir/PrivateFile (0700/0600 or
                                    protected DACL), ReservedName(name), FreeSpace(path)
internal/blockio/            NEW   Aligned(dev, sector, size): sector-aligned read-modify-write adapter around a raw device,
                                    page-aligned buffers, optional uncached verify handle (pure, tested with a fake)
internal/sdcard/
    facts.go                 NEW   Facts (platform-neutral disk description), Role flags, Ident(Facts)
    policy.go                NEW   Policy per OS + Evaluate(Facts, Context) error: THE guard decision, pure
    inventory.go             NEW   interface Inventory { List() ([]Facts, error); Inspect(path string) (Facts, error) }
    opener.go                NEW   interface Opener { Open(Facts) (Raw, error) } ; Raw = Device + Size + SectorSize + Close + Rescan
    transport.go             NEW   interface Transport { Do(ctx, Request, progress func(string)) (Response, error) }
    inventory_linux.go       MOVE  today's sysfs.go logic, now producing Facts
    opener_linux.go          MOVE  O_RDWR|O_EXCL, unmount, BLKRRPART
    transport_unix.go        MOVE  pkexec/sudo + stdin (backend.go today); macOS reuses the sudo path
    inventory_darwin.go, opener_darwin.go, plist.go (parser, untagged), transport_darwin.go           (T11.8)
    inventory_windows.go, opener_windows.go, winparse.go (byte-buffer parsers, untagged),
    transport_windows.go (UAC re-launch + named pipe)                                                (T11.7, T11.9)
    platform.go              NEW   newPlatform() selected by GOOS file suffixes: returns Inventory, Opener, Transport
    backend.go, helper.go, fat32.go, payload.go, toolkit.go   unchanged API: Backend, Request, Response, Device
internal/kit/xz.go           NEW   checked xz reader (4.3)
internal/camopt/             NEW   option catalog and engine (4.5)
internal/discover/           reshaped: per-OS interface filter and multicast sender (4.6)
internal/backuploc/          NEW   backup folder suggestion and validation (3.2)
internal/image/, internal/squashfs/   NEW (T11.12 to T11.16, gated) (4.8)
```

Rule: **no `//go:build` OS file may contain decision logic.** OS files only collect facts and perform actions.
Everything that decides, parses or formats lives in an untagged file and is unit-tested on Linux with fixtures.

### 4.2 Targets and flags

Targets: `GOOS` in {linux, windows, darwin} x `GOARCH` in {amd64, arm64}. Flags, unchanged:
`CGO_ENABLED=0 -trimpath -buildvcs=false -ldflags "-s -w -X main.version=<v>"`. Additions:

- exact Go toolchain pinned in `tools/build/deps.lock` (`GO_VERSION` becomes the exact version with per-host
  sha256, as for Zig); release builds refuse any other `go version` (2.2 row 27);
- no Windows resource file or manifest in this phase (no extra tooling; elevation is explicit through `runas`, 4.4.5);
- the Go linker ad-hoc signs `darwin/arm64` outputs by itself; do not post-process with `strip` or `lipo`
  without re-signing (an unsigned arm64 binary is killed by the kernel);
- the test hooks (`-tags akamtest`) never ship: release builds are checked to be built without the tag
  (`internal/testhook` test already exists, `build.sh test`).

Minimum operating systems: Windows 10 1809 or later (VT console, `ConPTY`; Windows 11 is the tested target);
macOS: whatever the pinned Go release notes require (12 Monterey or later at the time of writing; the dev who
pins the toolchain writes the exact figure in the README); Linux: any kernel with the existing requirements.

### 4.3 xz in process (R2)

Dependency: `github.com/ulikunitz/xz v0.5.17` (BSD-3-Clause; its `go.mod` says `go 1.20`; it pulls no other module).
Added to `tools/assistant/go.mod` and `go.sum` (two lines: module hash and go.mod hash).

**go.sum procedure (offline, then cross-checked):** the module is already in the project-local module cache
(`tools/.toolchain/go/mod/github.com/ulikunitz/xz@v0.5.17`, with its `cache/download` entries). Add it with
`GOFLAGS=-mod=mod GOPROXY=off GONOSUMDB=* go get github.com/ulikunitz/xz@v0.5.17` run with the module and build
caches under `tools/.toolchain/go/`, then `go mod tidy` offline. Because the checksum database is skipped offline,
the new `go.sum` lines are **not yet sumdb-verified**: the coordinator confirms them once online
(`go mod verify`, or `GOFLAGS=-mod=mod go mod download` with the default `GOSUMDB`, the way
`tools/build/bootstrap.sh:264-266` already downloads) before the commit is merged. `bootstrap.sh` needs no code change
(it runs `go mod download` over the whole `go.mod`, which now includes the module); its `--print-plan` output
and `tests/bootstrap_test.sh` are checked for any hard-coded module list (none found).

**API use:**

- `kit.XZReader(file string, lim XZLimits) (io.ReadCloser, error)`; `kit.Options.Decompress` keeps its signature
  (`verify.go:63`) and its default (`:167-168`) becomes `XZReader`, so `kittest` and the callers do not change shape.
  The `procReader` and the `exec` import disappear. The `Usage{"xz not found..."}` error disappears.
- `xz.ReaderConfig{SingleStream: true}`: a concatenation of streams or trailing garbage is refused.

**Caps (the point of R2), checked in this order, all before the first decompressed byte is produced:**

1. **Compressed size**: the file is already capped by the downloader (256 MiB, `MaxKitDownload`, `download.go:55`) and hashed before this point.
2. **Structure pre-scan** (reads only headers, about 100 bytes from the start and the end of the file): parse the
   12-byte stream header, the first block header and the stream footer with its index. Refuse unless: the
   index lists **exactly one block** (what `make_kit.sh:267` produces with `xz -9e -T1`), the block uses only
   the LZMA2 filter (no BCJ, no delta, nothing else), and the **dictionary size declared by that filter is at
   most 64 MiB** (`xz -9e` uses exactly 64 MiB; the library exports `lzma.DecodeDictCap` to decode the property
   byte). This is necessary because `ReaderConfig.DictCap` cannot lower a dictionary requested by the stream
   (`lzmafilter.go:68-77`: `if dc > config.DictCap { config.DictCap = dc }`) and the dictionary buffer is allocated
   from that value (`lzma/decoderdict.go:21-26`, `MaxDictCap` is about 4 GiB): without the pre-scan a few hundred
   bytes of input could request gigabytes of memory.
3. **Declared uncompressed size**: the index gives the uncompressed size of the block. Refuse early above
   `maxStream` (1 GiB, `verify.go:27`) and above the 200 MiB content cap when the sum is already known.
4. **Streaming cap**: the existing `limitReader` (`verify.go:315,386-403`) stays as the backstop in case the
   index lies; the tar layer keeps its entry (10000) and content (200 MiB) caps.
5. **Truncation and corruption**: the library's reader returns an error at the end of stream when the check
   (CRC64/CRC32/SHA-256 per the stream flags) fails or the stream is truncated. The reader wrapper must surface
   `io.ErrUnexpectedEOF` as an error, never as a clean end, and `readArchive` must keep draining to the end of
   the stream as today (`verify.go:383-388`).

**Peak memory** for a legitimate kit is about the dictionary (up to 64 MiB, usually less: `xz` shrinks the
dictionary to the input size) plus the tar buffers. Asserted in a test with `runtime.ReadMemStats`.

**`kittest`** (`internal/kit/kittest/kittest.go:26-28,121,218`): the helper that writes a test kit with the external
`xz -c -6` switches to `xz.NewWriter` (the library writer; `WriterConfig{DictCap: ...}`), `Available()` always
returns true, and the `t.Skip("skip: xz is not installed")` goes away. The parity tests against the bash
`verify_kit.sh` (`parity_test.go:25`) legitimately still need the external `xz`, `bash` and `ssh-keygen`
and keep skipping when absent: they check interoperability, not the product.

**Kit portability fixes owned by the same task (2.2 rows 3 and 4):**

- Symlinks: no host symlink is ever created. The two fixed links (`overlay/bin/dropbear` and `dropbearkey`
  pointing at `dropbearmulti`) are recorded in a sidecar file in the extracted kit (`.akam-links`, one
  `path target` per line, covered by the verification) and, on platforms where `os.Symlink` works, nothing
  else changes for the bash wizard that may read the same kit directory: so the rule is "try `os.Symlink`; if
  it fails with a privilege error, write a regular-file copy of the target and the sidecar line". Directory-mode
  verification accepts both forms.
- Modes: directory-mode verification compares modes only where the OS has them (not on Windows); the tar-mode
  check against the archive headers (`verify.go:357,362`) is the authority and is unchanged.
- Names: after the existing regex, `filepath.IsLocal(filepath.FromSlash(name))` is required (it rejects Windows
  reserved device names and drive-relative forms) before any path is built (`verify.go:436`).

### 4.4 SD card on three operating systems (R4)

#### 4.4.1 Principle

Today one struct, `sdcard.Sys`, both reads the Linux system and decides (`sysfs.go:42-310`). Split it:

```
 OS code (thin, manual-tested)              pure code (unit-tested on Linux)           OS code
 Inventory.List/Inspect  ->  []Facts  ->  Evaluate(Facts, Policy, Context) -> Candidate/refusal
                                          Ident(Facts) -> identity token
 Opener.Open(Facts)  -> lock, dismount, open raw -> blockio.Aligned -> FAT32/MBR writers -> read-back
```

`Facts` is a plain struct, JSON-serialisable so a test can load it from a fixture file:

```
Facts {
  OS            string      // "linux" | "darwin" | "windows"
  Path          string      // /dev/sdb | /dev/disk4 | \\.\PhysicalDrive2
  Name          string
  Whole         bool        // a whole disk, not a partition/slice
  Size          int64
  SectorSize    int         // logical
  Bus           string      // usb | sd | mmc | sata | nvme | scsi | raid | virtual | other | unknown
  Removable     Tri         // yes | no | unknown
  Internal      Tri
  Virtual       bool        // disk image, file-backed, loop, zram, storage spaces
  ReadOnly      bool
  Model, Vendor, Serial, WWN, Revision string
  LayoutID      string      // MBR signature, GPT GUID, or ""
  Mounts        []Mount     // node, mount point or drive letter, fs
  Roles         []Role      // boot, system, pagefile, swap, hibernation, esp, recovery, crashdump
  InStack       string      // reason when part of LVM/LUKS/RAID/dynamic disk/storage pool, else ""
  Dynamic       bool        // Windows dynamic disk
}
```

`Evaluate` returns the same refusal sentences as today where a rule is shared (tests of `guard_test.go` keep
passing unchanged on the Linux inventory) and new sentences for the other OSes. The helper (`helper.go`) calls
`Inspect` again on its own and compares `Ident` (identity token) with the one the parent sent: it never trusts
the parent's facts (unchanged principle).

#### 4.4.2 Common rules (every OS)

- A whole disk only. 128 MiB to 128 GiB. Logical sector size 512 (a card with 4096-byte logical sectors is
  refused: the FAT32 and MBR writers use a 512-byte constant, `fat32.go:35`).
- Never a disk that carries a system role (root, boot, system, pagefile, swap, hibernation), a volume of the
  repository, the user's home, the working directory, the temp directory, the folder of this executable, the
  state directory, the backup folder or the folder holding the image or toolkit files (`GuardPaths`).
- Never part of a storage stack (LVM, LUKS, RAID, device-mapper, dynamic disk, Storage Spaces, APFS container
  that backs a system volume).
- Never read-only/write-protected (reported with the reason "the card's write-protect switch").
- Identity token = size, model, vendor, serial, wwn/layout id, revision, device number (as today for Linux),
  compared by the parent right after the red box and again by the helper before the first write and after
  unmount.
- The same closed operation set (`list`, `prepare`, `verify`, `wipe`), the same read-back (the built-in FAT32
  reader re-reads the card and `VerifyTree` applies the checks of `sd_card.sh verify`; every byte sent is compared),
  and the same `wipe` zero-fill with a spot check of both ends.

#### 4.4.3 Linux (existing, refactored only)

Facts from `/sys/class/block`, `/proc/self/mountinfo`, `/proc/swaps` as today. Bus: `usb` when the sysfs path
contains `/usb`, `mmc` for `mmcblkN` of device type `SD`. Eligible: whole `sdX` removable USB or `mmcblkN` SD
(unchanged). Elevation: `pkexec` (graphical session) or `sudo`, request on stdin, minimal environment
(`backend.go:67-136`). No behaviour change; the refactor is proven by the existing `guard_test.go` and helper tests
passing byte for byte.

#### 4.4.4 macOS

Facts from two `diskutil` calls with `-plist` output (an OS-provided tool; the alternative, the
DiskArbitration C framework, needs cgo and is excluded by R1):

- `diskutil list -plist` -> `AllDisksAndPartitions` (device identifiers, partitions, mount points, APFS physical
  stores) and `WholeDisks`;
- `diskutil info -plist /dev/diskN` -> `WholeDisk`, `Internal`, `Removable`/`RemovableMedia`,
  `BusProtocol` (`USB`, `Secure Digital`, `PCI-Express`, `Apple Fabric`...), `VirtualOrPhysical`, `SystemImage`,
  `TotalSize`, `DeviceBlockSize`, `MediaName`, `IORegistryEntryName`, `DiskUUID`, `ParentWholeDisk`,
  `APFSPhysicalStores`;
- `diskutil info -plist /` to learn which whole disk backs the system volume (`ParentWholeDisk`), as a check
  independent of the mount-point scan.

`diskutil` plist output is XML: a minimal plist reader on `encoding/xml` (about 120 lines, untagged,
`plist.go`) parses it; no dependency. Policy:

| Rule | Condition |
|---|---|
| node | `^/dev/disk[0-9]+$` only; `/dev/diskNsM` slices and `/dev/rdisk*` as input are refused (the helper maps `diskN` to `rdiskN` itself) |
| whole, physical | `WholeDisk == true`, `VirtualOrPhysical == "Physical"`, `SystemImage != true` |
| bus | `BusProtocol` is `USB` or `Secure Digital`. Not `PCI-Express`, `Apple Fabric`, `SATA`, `Thunderbolt`, `Virtual Interface` |
| external | `Removable == true` **or** `Internal == false`. A built-in SDXC slot of a Mac may report `Internal: true` with protocol `Secure Digital`: accepted on the protocol alone (to confirm on hardware, test H2.4) |
| system | not the `ParentWholeDisk` of `/`; no partition mounted at `/`, `/System/Volumes/*`, `/Users`, `/private/var`, `/Library`; no APFS container that has a physical store on this disk and holds such a volume |
| guard paths | home, working directory, `$TMPDIR`, the executable's folder, state, backup and image folders are not under any mount point of this disk |
| size, sector | 128 MiB..128 GiB, `DeviceBlockSize == 512` |

Open and write: the helper (root) runs `diskutil unmountDisk force /dev/diskN`, re-checks, then opens
**`/dev/rdiskN`** (the raw, uncached node; reads through it are a genuine read-back) read-write. All I/O goes
through `blockio.Aligned` (raw nodes require 512-aligned offset and length). `fsync` and a clean close;
afterwards the system may auto-mount the new volume and drop `.fseventsd`/`.Spotlight-V100` entries on it: the
read-back is done **before** closing, and a card with such extra directories still verifies because `VerifyTree`
checks the required files and rejects only `*.bin` at the root. The screen tells the user to eject with Finder
or `diskutil eject` (the program runs `diskutil unmountDisk` at the end but does not eject).

Elevation: default is `sudo -- <self> sd-helper` with the request on stdin (identical to Linux; sudo's password
prompt uses the terminal, not stdin). Without a terminal the step refuses. Optional `AKAM_SD_ELEVATE=osascript`
uses `osascript -e 'do shell script "<self> sd-helper --socket <path>" with administrator privileges'` (AppleScript
cannot pass stdin): the parent first creates a Unix socket in a `MkdirTemp` directory (`0700`, random name),
listens, and the root helper connects; the helper verifies with `getpeereid`/`LOCAL_PEERCRED` that the peer is
the user who started the program (uid passed as an argument), reads one request, answers, and the parent
removes the socket. Arguments passed to `osascript` contain only the fixed executable path and a random hex
socket path (no user data, so no shell-quoting hazard). Permissions needed: none beyond admin rights;
Terminal does not need "Removable Volumes" or "Full Disk Access" for a raw device node (to confirm on hardware,
H2.6).

#### 4.4.5 Windows

Facts by `DeviceIoControl` on the physical drive and on volumes, all through `golang.org/x/sys/windows`
(already in `go.mod` as an indirect module, `CreateFile`, `DeviceIoControl`, `FlushFileBuffers`,
`GetSystemDirectory`, `GetWindowsDirectory`, `FindFirstVolume`, `GetVolumePathNamesForVolumeName`,
`CreateNamedPipe`, `GetNamedPipeServerProcessId`, `Token.IsElevated`, `KnownFolderPath` all exist in v0.50.0;
the `FSCTL_*`/`IOCTL_*` numeric constants are not in that package and are declared locally):

- enumerate `\\.\PhysicalDrive0..N` until open fails;
- `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceProperty`) -> `STORAGE_DEVICE_DESCRIPTOR`: `BusType`,
  `RemovableMedia`, vendor/product/revision/serial strings;
- `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`, `IOCTL_DISK_GET_LENGTH_INFO` -> size, logical sector size;
- `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` -> partition style, MBR signature or GPT GUID, partition types (dynamic-disk
  LDM types, ESP, MSR, recovery);
- `IOCTL_DISK_IS_WRITABLE`;
- volumes: `FindFirstVolume`/`FindNextVolume`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` on each volume gives its
  disk numbers (also for spanned volumes), `GetVolumePathNamesForVolumeName` gives drive letters and folder mount
  points;
- roles: the volume of `GetSystemDirectory`, of `GetWindowsDirectory`, `%SystemDrive%`, page-file volumes
  (registry `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PagingFiles`), and the
  volumes holding the guard paths.

The raw byte buffers returned by the IOCTLs are decoded by **untagged** functions in `winparse.go`
(`ParseDeviceDescriptor`, `ParseGeometryEx`, `ParseDriveLayoutEx`, `ParseDiskExtents`) so that tests feed them fixtures built from the documented
structure layouts on any OS.

Policy:

| Rule | Condition |
|---|---|
| node | `^\\\\\.\\PhysicalDrive[0-9]{1,3}$` only. A drive letter or volume path typed by the user is refused (letters are shown for information only) |
| bus | `BusType` is `USB` (7), `SD` (0xC) or `MMC` (0xD). Refuse SATA, NVMe (0x11), SCSI, RAID, iSCSI, SAS, virtual (0xE, 0xF: file-backed virtual and Storage Spaces), unknown |
| removable | `RemovableMedia` is informational: many USB readers and USB sticks report it differently. The bus type plus the size gate plus the system-role scan are the gate; the red box shows vendor, product, size and the drive letters so that the user can recognise the card |
| system | none of its volumes is the system, boot or page-file volume; none holds a guard path; not a dynamic disk, not Storage Spaces |
| layout | MBR, GPT or RAW; LDM partition types refused |
| size, sector | 128 MiB..128 GiB, logical sector size 512 |
| writable | `IOCTL_DISK_IS_WRITABLE` true |

Open and write, in this order (helper only, elevated):

1. Re-run the inventory for that disk number, compare `Ident`.
2. For every volume on the disk: open `\\.\X:` or `\\?\Volume{GUID}` with read/write, share read|write;
   `FSCTL_LOCK_VOLUME` then `FSCTL_DISMOUNT_VOLUME`; **keep the volume handles open until the end** so that the
   system cannot remount. Any failure (typically "in use": Explorer window, antivirus scan, an open file)
   aborts before writing, with the message naming the drive letter. Volumes on a disk with no mounted volumes
   (a raw card) need nothing here.
3. Open `\\.\PhysicalDriveN` through `windows.CreateFile` (`GENERIC_READ|GENERIC_WRITE`, share read|write,
   `FILE_FLAG_WRITE_THROUGH`), wrapped in an own `rawDev` type (not `*os.File`: `ReadFile`/`WriteFile` with an
   explicit offset through `OVERLAPPED`, no reliance on `os.File` seek semantics on device handles).
   `FSCTL_ALLOW_EXTENDED_DASD_IO` on the handle.
4. All I/O through `blockio.Aligned` (offset and length multiples of the sector size; read-modify-write at the
   edges).
5. After the MBR and the volume are written: `FlushFileBuffers`, `IOCTL_DISK_UPDATE_PROPERTIES` so that the
   system re-reads the layout, then verification through a **second, uncached handle** (`FILE_FLAG_NO_BUFFERING`
   with page-aligned buffers from `blockio`), because reading through the write handle can return the cache:
   this replaces Linux's `BLKFLSBUF` (`os_linux.go:11-17`).
6. Release the volume locks last. The system then mounts the new volume under a drive letter.

BitLocker To Go cards and cards where `FSCTL_LOCK_VOLUME` fails are refused with a reason; no attempt is made to
suspend protection.

Elevation (UAC). A process that is not elevated (`Token.IsElevated()` false) re-launches itself with the
`runas` verb through `ShellExecuteExW` (loaded with `syscall.NewLazyDLL("shell32.dll")`: `windows.ShellExecute`
exists in `x/sys` but returns no process handle and `ShellExecuteEx` is not provided; the wait is done on the
pipe, below). `ShellExecute` cannot hand stdin or stdout to the new process, so **the request travels over a
named pipe**, designed as follows (T11.9):

- The parent creates `\\.\pipe\akam-sd-<32 hex random>` **before** launching, with `PIPE_ACCESS_DUPLEX`,
  `FILE_FLAG_FIRST_PIPE_INSTANCE` (nobody can have created it first), `PIPE_REJECT_REMOTE_CLIENTS`, one instance,
  and a security descriptor whose DACL grants access **only to the current user's SID** (and SYSTEM). An
  elevated process of the same user (the normal UAC case) can connect; nobody else can.
- The elevated process is started as `akam.exe sd-helper --pipe <name> --parent <pid>` hidden (`SW_HIDE`). It
  connects within 30 s or exits. It checks `GetNamedPipeServerProcessId == <pid>` before reading, so a pipe
  squatted by another process is not obeyed.
- Framing: the parent sends one JSON request (same `Request` type, same 96 MiB cap, `DisallowUnknownFields`,
  one document); the helper answers with zero or more `{"progress":"..."}` lines and one final `Response`, then
  exits after a single operation. Progress is thus shown by the parent's own UI, where on Linux it goes to stderr.
- **The Wi-Fi password and every file of the card travel only on the pipe, never on disk and never on the command
  line.** (The alternative suggested in the brief, a request file in a private temp directory with a nonce and an
  HMAC, would write the password to a file that the elevated helper, possibly running as a different
  administrator account in the "over-the-shoulder" UAC case, may not even be able to read; it needs a cleanup
  that fails on a crash. The pipe has neither problem. If the coordinator prefers a file, the fallback design is in Q3.)
- The 32-hex random in the pipe name acts as a bearer nonce against processes that cannot read the DACL-protected
  pipe object anyway; the helper's own guards (4.4.2) are the real authority, because a forged request can at worst
  write a disk that passes every guard **and** matches the identity token the user confirmed.
- If the user declines the UAC prompt, `ShellExecuteExW` returns `ERROR_CANCELLED`; the step ends with "no
  administrator rights: nothing was written". If the helper cannot be launched at all (Group Policy), the message offers
  "start Akam as Administrator" (right click, Run as administrator): the same binary, `Token.IsElevated()` is
  true, the helper runs in process, no pipe. The UAC dialog shows "Unknown publisher" until the binary is
  signed (8.3).

#### 4.4.6 `blockio.Aligned`

A pure adapter implementing `sdcard.Device` over a `Raw` with `SectorSize`: whole-sector reads and writes pass
through; partial sectors at either end are read, patched and written back; buffers handed to the OS are
page-aligned. Tested with an in-memory fake that returns `EINVAL` for any unaligned call, then runs the whole
FAT32/MBR writer through it and compares with the direct write on a plain file.

#### 4.4.7 `Opener` and `Transport` summary

| | Linux | macOS | Windows |
|---|---|---|---|
| node written | `/dev/sdX`, `/dev/mmcblkN` (`O_RDWR|O_EXCL`) | `/dev/rdiskN` | `\\.\PhysicalDriveN` |
| unmount | `umount2` per mount point | `diskutil unmountDisk force` | `FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME` per volume, handles held |
| re-read table | `BLKRRPART` | the OS re-probes on close | `IOCTL_DISK_UPDATE_PROPERTIES` |
| uncached verify | `BLKFLSBUF` | raw node is uncached | second handle `FILE_FLAG_NO_BUFFERING` |
| elevation | `pkexec`/`sudo`, stdin | `sudo`, stdin (default); `osascript` + Unix socket (opt-in) | UAC `runas`, named pipe |
| progress | stderr of the child | stderr / socket | pipe lines |

#### 4.4.8 Testability on Linux for all three OSes (R4)

- `Evaluate` is table-tested with JSON `Facts` fixtures in `internal/sdcard/testdata/facts/{linux,darwin,windows}/*.json`:
  each file is one disk with an expected verdict and an expected refusal substring. Required cases per OS are listed in 7.1.
- Parsers are fed fixtures: macOS `diskutil list -plist` and `info -plist` XML files (synthetic first, then captured
  from a real Mac by the hardware tester and committed after review, removing serial numbers); Windows IOCTL
  buffers built by a test helper that writes the documented struct layouts.
- The helper is exercised through a fake `Opener` that records calls, so the **order** of operations is asserted on
  every OS (re-inspect, compare identity, lock/unmount, re-check, open, write, flush, verify, release).
- `GOOS=windows go vet ./...` and `GOOS=darwin go vet ./...` run in `tests/assistant_test.sh` (7.2): they type-check
  every OS file on a Linux host.
- Not testable on Linux, therefore in the hardware plan (7.3): the IOCTL calls themselves, UAC, pipe ACLs,
  `diskutil` behaviour, the real raw write.

#### 4.4.9 Escape hatch: files-only mode (all OSes)

If a platform cannot write raw (policy forbids elevation, no admin rights), `--files-only DIR` writes the card
**tree** to a folder and prints the exact steps: format the card FAT32 with the OS tool, copy the folder
contents to the card root, run `akam verify-sd --mount PATH` (the read-only checks of `VerifyTree` on a mounted
card; a mounted-path `Tree` already exists as `DirTree`, `payload.go:188-214`). This is spec 09's original
Windows plan, kept as the documented fallback, not the default.

### 4.5 Camera options engine (R3c)

`internal/camopt` is a catalog plus a small engine; the commands `led`, `audio`, `picture`, `debug`, `onvif`,
`hostname`, `rtsp-auth` and the new `video`, `ntp` and `audio mic` become thin front ends of it, so the option
screens (3.4) and the command line share one set of validators and one apply path. The golden tests
(`testdata/golden/*.golden`, made from the bash wizard) keep guarding the command-line text.

```
Option {
  ID, Group, Label, Help string
  Kind        enum | int | bool | string | secret | action
  Values      []string  or  Min, Max int
  Read        how to read it: ctl GET key(s) | helper `status`
  Write       how to write it: ctl SET key value | helper command with fixed words + validated argument
  Persisted   camera-saved | RAM-only (until reboot)
  Applies     now | video-restart | next-power-cycle
  Danger      none | asks (yes/no default No) | red box
  Needs       other option or condition (ONVIF needs the stream password)
}
```

Catalog (from the commands that exist today, `./akam help` and `internal/cli/help.txt`):

| Group | Option | Values | Mechanism (existing camera side) | Saved | Applies |
|---|---|---|---|---|---|
| LED and night | front LED | on, off, auto | ctl `led.status` | camera | now |
| LED and night | IR night LEDs | auto, off | ctl `night.ir_led` | camera | now |
| LED and night | day/night mode | auto, day, night | ctl `night.mode` | RAM | now |
| Picture | brightness, contrast, saturation, sharpness | -50..50 | ctl `isp.*` | camera | now |
| Picture | white balance | conf, manual | ctl `img.wb` | RAM | now |
| Audio | speaker volume | 0..100 | ctl `audio.out_volume` | camera | now |
| Audio | speaker | on, off | ctl `audio.out_enable` | camera | now (may need a reboot to start) |
| Audio | talk (two-way) | on, off | ctl `audio.talk_enable` | camera | now |
| Audio | microphone | on, off | ctl `audio.mic_enable` (bash: `steps_camera.sh:1442`) | camera | next power cycle |
| Video | bitrate | 500..3000 kbit/s | ctl `video.bitrate_cfg` | camera | video restart |
| Video | frame rate | 10, 15 | ctl `video.fps_cfg` | camera | video restart |
| Video | key frame interval | 1..4 s | ctl `video.gop_cfg` | camera | video restart |
| Security | stream password | set (typed twice), disable | `akam_rtsp_auth.sh set-creds` on stdin / `disable` | camera | now |
| Network | ONVIF | enable (port), disable | `akam_onvif.sh` | camera | now; needs the stream password |
| Network | NTP | up to 3 servers, off | `akam_ntp.sh set/off/status` | camera | now |
| Network | host name | RFC 1123 label, max 32 | `akam_hostname.sh set` | camera | next power cycle |
| Network | Wi-Fi | **read-only** (SSID, signal) | ctl `net.ssid`, `net.wifi_rssi` over the SSH loopback | n/a | n/a (Q7) |
| Advanced | debug mode (8091 writable from the network) | on (1..1440 min), off | `akam_debug.sh` | RAM | now |

Engine behaviour: one SSH session per screen; all `GET`s of a group in one control-port exchange
(`ctlExchange`, `ctlcmds.go:23`); reads never need confirmation; writes go through the existing `a.run` guard
(`app.go:500`, `remote.go:57`) so the "no reboot/flash/kill" rule still holds by construction; the engine
renders `Applies` and `Saved` in the result. `INFO <key>` (API v2) is used when available to check `min`/`max`
against the catalog and to hide options the camera does not have.

New commands to reach parity with the bash wizard (T11.6): `video [show | bitrate N | fps N | gop N]`,
`ntp [status | set SERVER... | off]`, `audio mic on|off`, with the same texts and golden files as bash
(`./akam help` lines 88-102 of `akam`).

### 4.6 Discovery and remembered cameras (R3b)

`internal/discover` gains an orchestrating function `Run(ctx, Plan) []Camera`; `findcmd.go` and the new screen
call it. The sources, the opt-in rule and the TCP/UDP details:

- **Known cameras**: `state.Cameras` already stores one state directory per camera with `camera_ip` and
  `camera_kind` (`internal/cli/cameracmds.go`, `state.go`). New keys, ignored by the bash wizard (extra keys are
  ignored, spec 09 T1.2): `camera_host`, `camera_fw`, `camera_seen` (UTC time). `akam cameras` shows them.
- **Probe**: `OpenPorts` on 24, 22, 554, 8091, 1.5 s, parallel (existing). `Classify` (existing) gives stock+card /
  Akam / partial / unknown.
- **Identify (read-only, Akam only)**: when 8091 is open, one TCP session sends `GET fw.version`, `GET sys.hostname`,
  `GET net.ip` and `QUIT` and reads at most 4 lines of at most 255 bytes each, with a 2 s deadline. No `HELLO`, no `SET`,
  no SSH. A camera that answers `ERR` for `sys.hostname` (older image) is still listed with its firmware.
  A stock camera is never driven: its telnet shell is not touched during discovery.
- **Name**: `net.LookupHost`, then mDNS `<name>.local` (`MDNSLookup`, existing).
- **WS-Discovery** (existing `WSDiscover`): best effort, see the firewall note below.
- **Scan**: `Subnets` + `Scan` (existing), 64 concurrent dials, 2 s, behind the explicit yes/no.
- **Per-OS fixes** (T11.10): (a) the virtual-interface filter becomes a pure function on a small `Iface` struct
  (name, flags, hardware address, addresses) with OS-specific name rules: Linux as today; Windows names such as
  `vEthernet (...)`, `Loopback Pseudo-Interface`, `VMware Network Adapter`, `VirtualBox Host-Only`, `Hyper-V`,
  `Bluetooth Network Connection`, `isatap`, `Teredo`; macOS `lo0`, `utun*`, `awdl0`, `llw0`, `bridge*`, `gif0`,
  `stf0`, `ap1`, `anpi*`, `vmenet*`; link-local `169.254/16` is skipped everywhere; (b) multicast is sent **once
  per eligible interface** from a socket whose outgoing interface is set (`IP_MULTICAST_IF` through
  `net.ListenConfig.Control`, one small file per OS family because the descriptor type differs), instead of the
  OS-default interface; (c) the scan range refuses subnets larger than /22 unless the user typed the range, and a
  /23 or /22 shows its size in the question.
- **Firewall note (Windows, macOS)**: unsolicited UDP replies to the WS-Discovery probe can trigger the
  "Allow Akam on private networks?" firewall prompt on the first run. The TCP scan only makes outgoing connections
  and never triggers it, so on Windows and macOS the TCP scan is the primary method and WS-Discovery stays an
  explicitly separate yes/no with a sentence about the prompt.
- **Untrusted by nature**: any LAN host can answer. Results are hints. Nothing is written to a camera on the
  strength of discovery: SSH host-key pinning with the first-contact fingerprint (existing) and, for stock
  cameras, the `/proc/mtd` gate (existing) are what authenticate the target. All strings from the network pass
  `ui.Sanitize` (section 5.4).

### 4.7 Backup location

Package `internal/backuploc` (pure, with the OS facts injected): `Suggest(osFacts, camera, now) Suggestion`,
`Validate(path, osFacts) []Problem`, `NextFree(base)`. `bkPCDir` (`telnetcmds.go:409-425`) calls it; its
`AKAM_BACKUP_ROOT` test hook keeps working (legacy `<root>/akam-backup-<name>-<date>` layout). The default folder is `<Documents>/Akam backups/<camera>/<date>[-N]`; `Suggestion.Parents` lists the two folders above it, which the caller makes private (`fsutil.PrivateMkdirChain`) before the date folder (`fsutil.PrivateMkdirNew`). Rules in 3.2.

### 4.8 Native image builder (the dependency behind R1 and R3(d))

Facts: the image step is `mtdblock7.bin` (stock squashfs) -> unpack -> copy the kit overlay -> apply shell patches
(`tools/firmware_patch_templates/patch_service_sh.sh`, 209 lines, `patch_ethernet_hostname.sh`, 81 lines) ->
safety checks -> `mksquashfs -comp xz -b 131072 -all-root -noappend -no-xattrs` -> round-trip verify
(`src/ak_rtsp/build_firmware.sh`, 698 lines; spec 09 10.2 records the exact reference layout, including the
**export table** and fragments, which only the stock-equal layout is known to boot).

Plan, gated so that nothing unproven ships:

1. **Reader** (T11.12): squashfs 4.0 superblock, inode/dir/fragment/id/export tables, metadata blocks, data
   blocks, xz and zlib (gzip) decompression (`ulikunitz/xz` and the standard library; lzo or zstd stock images are
   refused with the existing `--allow-unknown-stock-comp` wording). Cap: image size 5 MiB (the partition),
   inode count, path depth, per-block and total expansion caps (it parses untrusted data from the user's own
   backup). Acceptance: lists and extracts `tests/fixtures/app_fake*` identically to `unsquashfs -lls`.
2. **Writer** (T11.14): the subset above (4.0, xz blocks of 131072, all-root, fixed time, fragments, id table,
   export table, no xattrs). xz specifics to validate in the first hours of the task: the kernel's xz decoder
   wants a **CRC32 check** (`xz.WriterConfig{CheckSum: xz.CRC32}`) and a **dictionary no larger than the block
   size** (`DictCap: 131072`), as `mksquashfs` writes them; verify against the camera kernel's squashfs config
   (spec 09 10.2 records the reference layout; the camera kernel's squashfs/xz options must be read from its config before coding). Acceptance: `unsquashfs -s -lls` of the output equals the
   `mksquashfs` result on the fixtures except for allowed ordering and compression bytes; a loop mount in a
   privileged CI/manual run lists the same tree; size <= 5242880.
3. **Assembly** (T11.16): Go port of the patch steps and of `build_firmware.sh --kit` (overlay copy, modes, the
   two patches, report fields, checks), with golden comparison against the bash output on `tests/fixtures/app_fake*`
   and `app_fake_variants`. Commands: `image` (native). `--builder tools` keeps the bash path on Linux.
4. **Gate** (human, not a task): the maintainer builds the image of a real dump with both builders, flashes one
   spare camera with the **native** image over the reviewed path, and records the result (`docs/akam_v1_test_plan.md`).
   Until the gate passes, `image` on Windows and macOS stays "build it on Linux (or WSL) and give `--image FILE`".
   After the gate, the default changes. The writer never becomes the default on Linux before the gate either.

If the gate fails or the effort is judged too high (Q1): fall back to shipping static `mksquashfs`/`unsquashfs`
per host in the kit (spec 09 10.1 fallback), with the Windows and macOS builds of those tools being the hard part.

### 4.9 Other platform details (R5)

#### 4.9.1 State lock (Windows)

`internal/state/lock_windows.go`: `windows.LockFileEx` on `<state>/.akam.lock` with
`LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY`, `UnlockFileEx` on release; `ERROR_LOCK_VIOLATION` means "held by another run"
(same `ok=false` contract as `lock_unix.go:16-33`). `lock_other.go` becomes `!unix && !windows` (plan 9 and others:
no-op, documented). A test starts a second process (re-exec of the test binary) and expects refusal.

#### 4.9.2 State and paths

- Linux: unchanged, `~/.local/state/akam`, `XDG_STATE_HOME`, `AKAM_STATE_DIR` (shared with the bash wizard).
- macOS: `~/Library/Application Support/Akam` (`os.UserConfigDir()` on darwin), because the bash wizard is Linux only
  (`tools/build/bootstrap.sh:53`); `AKAM_STATE_DIR` and `--state-dir` still win.
- Windows: `%LocalAppData%\Akam`, **not** `os.UserConfigDir()` (that is `%AppData%`, which roams between machines
  in a domain: host-key pins, logs and the kit cache must not roam). `AKAM_STATE_DIR`/`--state-dir` win.
  No migration is written: a binary started on a new OS starts fresh.
- SSH key: `~/.ssh/akam` becomes `<home>\.ssh\akam` on Windows through `os.UserHomeDir()` (`%USERPROFILE%`), same
  file names as the OpenSSH for Windows client uses, so `ssh -i` works there too. Camera host-key pins stay in
  the state directory's own `known_hosts` (never `~/.ssh/known_hosts`, as today).
- Home-relative display: paths shown to the user are absolute with OS separators; the state file keeps the
  absolute path it was given.
- Windows paths longer than 260 characters use the `\\?\` prefix when built by the program (backup folder names
  are short by construction; a user-chosen deep folder is checked).

#### 4.9.3 Permissions (Windows has ACLs, not chmod)

What the code does today: `0o700`/`0o600` in `os.MkdirAll`/`os.WriteFile` (2.2 row 12). On Windows Go ignores the
bits. The rule adopted:

- state directory, `.ssh` directory, backup folder, run/temp directories created by the program: created through
  `fsutil.PrivateMkdir`, which on Windows sets a protected DACL (user, SYSTEM, Administrators) with
  `SetNamedSecurityInfo`; files created inside inherit it. Failure is a warning in `doctor` and in the log, not a
  hard error (a FAT/exFAT or network path cannot hold ACLs).
- the private key `~/.ssh/akam`: created inside the (private) `.ssh` directory; the program also applies the same
  protected DACL to the key file itself, because OpenSSH for Windows refuses keys that other accounts can read.
  The Go SSH client itself never checks permissions.
- the Wi-Fi password is never stored by the program (unchanged); it only exists in memory and on the card.

#### 4.9.4 Console, colours, Ctrl-C

- At startup, `platform.EnableVT()` calls `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on stdout and
  stderr (the constant exists, `x/sys/windows/types_windows.go:2506`); on failure the plain UI runs with
  `Color=false` (fix of `ui/plain.go:54-59` and `app.go:253`). `NO_COLOR` is honoured as today. bubbletea enables its
  own VT mode for the full-screen UI. No call to `SetConsoleOutputCP` is needed: Go writes to a real console in UTF-16.
  Redirected output stays UTF-8.
- `golang.org/x/term.ReadPassword` already works on the Windows console (no echo).
- Ctrl-C: `os.Interrupt` works on all three. SIGHUP does not exist on Windows; the console-close event arrives as
  SIGTERM, and Windows terminates the process a few seconds later whatever the handler does. The signal sets move
  into `platform.SignalsToIgnore()` so `sdcmds.go:28-31` and `talkcmds.go:131` do not name `SIGHUP` on Windows. What
  protects a flash on every OS is not signal handling but the existing design: the camera-side worker is detached and the
  message says not to power-cycle. A closed window during an **SD write** leaves a half-written card: the next
  run's verify refuses it, nothing else is harmed.
- Minimum console: Windows 10 1809+ (VT). Legacy `cmd.exe` on older Windows is unsupported and `doctor` says so.

#### 4.9.5 Line endings

All text the program writes for the camera or the card is LF only (the wifi.conf, the entry hooks, authorized_keys,
embedded scripts, SHA256SUMS). Two rules: (1) `.gitattributes` gains `*.sh text eol=lf`, `*.embed text eol=lf`,
`tools/assistant/internal/assets/scripts/** text eol=lf`, `*.txt text eol=lf` for the shipped lists, so a Windows
checkout never changes embedded scripts (T11.2, with a test that scans `internal/assets/scripts` for `\r`);
(2) **input** is tolerant: a public key file, a `--password-file`, an `allowed_signers` file or a pasted fingerprint
may end in `\r\n` and the readers strip the `\r` (`CheckPubKey`, `payload.go:54`, the password reader, `--trust-new-host-key`
compare). `SHA256SUMS` is written as `<hash>  <name>\n` (two spaces, LF) so `sha256sum -c`, `shasum -c` and the
built-in check all accept it.

#### 4.9.6 Audio and talk (R5 decision)

Recommendation, realistic scope: **`talk` stays feature-gated per platform; nothing new is built in this phase.**

- `talk test` (generated tone) and `say FILE.wav` are pure Go and work on all six targets (existing; spec 09).
- Microphone capture and non-WAV decoding need an external program: Linux `arecord`/`ffmpeg`/`pw-record`
  (existing), macOS and Windows `ffmpeg` only. `doctor` shows the status per capability
  ("microphone: ffmpeg found / not found - install ffmpeg to use `talk`").
- Fix a real bug (T11.13): `ffmpeg -f dshow -i audio=default` (Windows, `capture.go:75-77`) is not a valid
  device name. The capture code lists devices (`ffmpeg -list_devices true -f dshow -i dummy`, parsed) and, with
  more than one microphone, asks which one (`UI.Pick`); `avfoundation` `:default` (macOS) is valid as is.
- Pure-Go capture: **Windows** is feasible without cgo and without a new module (`winmm.dll` `waveIn*` through
  `syscall.NewLazyDLL`, about 250 lines) and is the recommended *next* step if users ask; **macOS** (CoreAudio) cannot
  be done without cgo or a purego-style module, which R1/R2 do not authorise; **Linux** ALSA by raw ioctls is large.
  Not in this phase (Q6).
- `internal/audio/proc_other.go`: the process-group kill for the ffmpeg child on Windows uses a Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so the child dies with the parent (parity with `Pdeathsig` on Linux);
  on macOS, `Setpgid` and a kill of the group on every exit path (no `Pdeathsig`; documented limit).

### 4.10 Size budgets and reductions (R6)

Measured baseline: 2.3. Proposed budgets (hard limits checked by `build.sh sizes` and `tests/assistant_test.sh`):

| Target | Measured today | Budget now | Soft warning |
|---|---|---|---|
| linux/amd64 | 9.59 MiB | 12 MiB | 11 MiB |
| linux/arm64 | 8.88 MiB | 12 MiB | 11 MiB |
| darwin/amd64 | 9.78 MiB | 12 MiB | 11 MiB |
| darwin/arm64 | 9.09 MiB | 12 MiB | 11 MiB |
| windows/amd64 | 9.93 MiB (the user's 14.6 MiB is not reproduced) | 12 MiB | 11 MiB |
| windows/arm64 | 9.06 MiB | 12 MiB | 11 MiB |

One number for all targets keeps the rule simple. Expected growth from this phase: xz decoder and encoder
(+0.2 to 0.4 MiB), Windows/macOS disk code (+0.1), options/discovery/UX (+0.2), squashfs reader/writer and
image assembly (+0.4 to 0.7): the sum stays under 11 MiB on the measured baseline, which is why the soft warning is 11. If
T11.5 reproduces a figure above 12 MiB for Windows with the pinned toolchain, it first finds the cause (section 2.3
checklist) and only then proposes a per-target budget with the reason written next to it.
The archive budget stays 5 MiB per target (spec 09 7.4). Add: start to menu < 100 ms on Windows (cold, with
antivirus), checked on the hardware plan.

Reductions, in the order to try (each one measured and kept only if it saves more than 100 KiB):

1. Confirm the flags are really applied in every path that produces a release (`-s -w`, `-trimpath`, no
   `-tags akamtest`): they are in `build.sh:36-38` already; the release script must reuse that function (T11.5).
2. `-tags nethttpomithttp2` (an existing `net/http` build tag, to be confirmed on the pinned toolchain) drops the
   bundled HTTP/2 client (about 120 KiB of `net/http/internal/http2` in an unstripped Windows build here); the kit and toolkit downloads work
   over HTTP/1.1.
3. Per-package size report (`go tool nm -size` on an unstripped build, grouped by import path) added to
   `build.sh sizes --by-package`, to see whether `x/text`, `lipgloss`/`termenv`/`colorprofile`, `net/http` or
   `x/crypto/ssh` dominate. If `x/text` is only pulled in for width tables of `go-runewidth`, check whether a
   leaner path exists in the TUI code before touching the dependency (the dependency set is the user's decision, spec 09 0.2).
4. Embedded camera scripts (about 1.8k lines, around 90 KiB) could be stored compressed and decoded at start with the in-process xz;
   **not** worth it (about 50 KiB, adds start-up work): rejected unless step 3 shows nothing else.
5. **Do not use UPX**: unpacking at start costs time and memory, defeats reproducible verification of the shipped
   file and is a classic antivirus false-positive trigger on Windows.

---

## 5. Security

1. **Elevation and request passing**: 4.4.4 and 4.4.5. The main process is never elevated; only `sd-helper` is,
   per operation; the request carries no executable path or command; the closed operation set stays
   (`list`, `prepare`, `verify`, `wipe`); the helper reads no file from disk (the card content travels in the
   request); one request, one answer, exit. Windows: DACL-restricted first-instance named pipe, server-PID check,
   30 s connect deadline. macOS: root helper checks peer credentials on the socket, or sudo with stdin. Linux:
   unchanged.
2. **Device guards**: 4.4.2 to 4.4.5. The helper recomputes facts itself. Never trust a path from the camera or a
   file; a user typing a device must pass the same `Inspect`/`Evaluate`. Windows: only `\\.\PhysicalDriveN`, by
   bus type and system-role scan; macOS: whole physical external/SD disks, plus an independent check against the
   disk behind `/`. Every OS: the red box shows device, size, model; a second, independent identity comparison
   happens inside the elevated process after unmount.
3. **Secrets**: Wi-Fi password, stream password, SSH passphrase read without echo (`x/term`), never on a command
   line (`osascript`/`ShellExecute` arguments contain only a random pipe or socket name), never on disk outside the
   card, never in the state or log (existing redaction `state.Secrets`); on Windows never in a request *file*.
4. **Camera-controlled strings are data, never code, never paths, never terminal control**: hostname, firmware
   version, SSID, telnet and SSH banners, `LIST`/`GET` values, WS-Discovery XML fields.
   - a single `ui.Sanitize(s)` (strip C0/C1 controls including ESC, drop bidi override characters U+202A-202E and
     U+2066-2069, collapse newlines, cap at 64 runes) is applied by the UI layer to every string that originates from
     the network before it is shown, so an ANSI sequence in a hostname cannot rewrite the TUI or the plain output;
   - no such string is ever concatenated into a shell command: the catalog (4.5) builds commands from fixed words
     and arguments validated by regexes (`hostnameValid`, NTP name/IPv4 grammar, numeric ranges);
     the existing `shq` (`payload.go:51`) is used only for fixed, program-built paths; the `a.run` guard stays;
   - no such string builds a host path: backup and state folders use the user's validated camera name; a discovered
     hostname offered as the *suggested* camera name is first normalised to the name grammar (lowercase, other
     characters to `-`, trimmed, reserved Windows device names refused, `NameMax` 20) and shown for confirmation;
   - length caps: 255 bytes per control-port line, 4 lines for identification, 64 KiB per telnet line (existing).
5. **Kit and archive**: sha256, SSHSIG, layout checks unchanged; xz caps (4.3); `filepath.IsLocal` on extraction;
   no host symlink created (4.3); a kit directory on Windows never relies on file modes.
6. **Backups are secret-bearing** (Wi-Fi in CONFIG): private permissions (3.2, 4.9.3), cloud-folder warning,
   `README.txt` in the folder says so (text exists today; one sentence added).
7. **Network exposure**: none listening. WS-Discovery and subnet scans only after a yes (default No). Discovery
   reads port 8091 with a read-only command set (`GET`, `QUIT`): a test asserts that no `SET`, `SAVE`, `RESET`,
   `HELLO` or anything but those words is ever sent by discovery.
8. **Destructive steps**: unchanged and re-asserted by tests: single explicit yes/no (default No), red box, no
   terminal means no action, `flash.Confirmation` bound to the image md5, backup gate re-verified at flash time,
   no `reboot`/`poweroff`/`kill`/watchdog on any path (the video restart is the one audited exception, Q8).
9. **Supply chain**: `go.sum` pins (including the new xz lines, sumdb-confirmed once online), pinned exact
   toolchain, reproducible builds, release checksums and SSHSIG, notices shipped with every artifact. No auto-update.
10. **Windows and macOS binary trust** (unsigned for now): published SHA256SUMS and SSHSIG, instructions in 8.3, and
    `akam verify-release` (Q10) to check another downloaded binary against the signing key built into this one.
11. **Temp files**: private directories under the state directory (`MkdirTemp` + `PrivateMkdir`), removed on exit and on
    signals; on Windows removal retries (AV scanners hold files briefly).

---

## 6. Work breakdown

Sixteen tasks, each sized for one agent (a few hours to a day of focused work, one reviewable merge), with owned files,
dependencies and acceptance criteria (AC). A pair that runs in parallel never edits the same file. Every task ends with
`tools/assistant/build.sh test` (vet and tests, with `-tags akamtest`) green, `gofmt` clean, and
`tests/assistant_test.sh` green on Linux; tasks from T11.5 on also run `GOOS=windows`/`darwin` vet. Language of every
text in the tool: English. No task touches the camera or the network.

Suggested schedule (two at a time; a pair never shares a file):

```
S1  T11.1 xz + kit portability          |  T11.2 platform hygiene (state, lock, console, keys, line endings)
S2  T11.3 SD abstraction refactor       |  T11.4 backup location + private folders
S3  T11.5 build, cross-compile, release |  T11.6 camera options engine + video/ntp/mic commands
S4  T11.7 Windows SD disk layer         |  T11.8 macOS SD disk layer + elevation
S5  T11.9 Windows UAC transport         |  T11.10 discovery and known cameras
S6  T11.11 backup over SSH              |  T11.12 squashfs reader
S7  T11.13 guided flows, home, licences |  T11.14 squashfs writer
S8  T11.16 image assembly + image cmd   |  T11.15 docs, hardware plan, release notes
```

### T11.1 - xz in process and kit portability
- Files: `tools/assistant/go.mod`, `go.sum`, `internal/kit/xz.go` (new), `internal/kit/verify.go`, `internal/kit/kittest/kittest.go`,
  `internal/kit/*_test.go`, `internal/cli/misccmds.go` (remove the `opt("xz", ...)` doctor line only),
  `tools/assistant/README.md` (the sentences about `xz`), `THIRD_PARTY_NOTICES.md` (row for the module).
- Deps: none. Needs the module cache (no download). Coordinator re-checks `go.sum` online once.
- Content: 4.3 in full (dependency, pre-scan, caps, `kittest` writer, symlink fallback with `.akam-links`, mode checks off on
  Windows, `IsLocal`). Notices row: `github.com/ulikunitz/xz | v0.5.17 | BSD-3-Clause | in-process decoding of the kit (.tar.xz)`.
- AC: no `exec` of `xz` in non-test code (`grep`); unit tests (no external `xz`): valid kit decodes; truncated stream errors;
  bit-flipped stream errors (check mismatch); concatenated streams refused; multi-block stream refused; a hand-built header
  declaring a 4 GiB dictionary refused **without allocating** (assert with `runtime.MemStats` delta < 128 MiB); a 64 MiB
  dictionary (what `xz -9e` writes) accepted; a stream whose index declares more than 1 GiB refused; a lying index caught by
  `limitReader`; peak memory of a legitimate kit decode < 100 MiB; extraction with `os.Symlink` forced to fail (injectable)
  produces the copy + sidecar and passes directory-mode verification; a tar entry named `NUL`/`con` refused on a `filepath`
  fake for Windows (inject `isLocal`); parity tests still pass when the external tools exist. `kittest.Available()` true everywhere.

### T11.2 - Platform hygiene
- Files: `internal/platform/*` (new), `internal/fsutil/*` (new), `internal/state/lock_windows.go` (new), `lock_other.go` (tag),
  `internal/state/state.go` (default dir), `internal/state/cameras.go` (`NameValid` reserved names), `internal/camera/ssh/keys.go`
  (agent over named pipe on Windows, private key file), `internal/camera/ssh/client.go` (private dir/file helpers),
  `internal/ui/plain.go` and `internal/cli/app.go` (VT enable / colour fallback), `internal/cli/sdcmds.go` and `talkcmds.go`
  (signal sets only), `cmd/akam/main.go` (call `platform.Init`), `.gitattributes`, `internal/audio/proc_other.go`
  (+ `proc_windows.go`, `proc_darwin.go`), tests.
- Deps: none.
- Content: 4.9.1 to 4.9.5 and the audio process rule of 4.9.6. `NameValid` additionally refuses the Windows reserved names
  (`con prn aux nul com1-9 lpt1-9`, case-insensitive) on every OS, so a camera name works on any machine the backup moves to;
  existing cameras with those names are not renamed (documented, vanishingly unlikely). Atomic file replace with retries
  in `fsutil.AtomicWrite` (state writes `os.Rename` over an open file fail with sharing violations under AV).
  SSH agent on Windows: `os.OpenFile` of `\\.\pipe\openssh-ssh-agent` wrapped as the `io.ReadWriter` that `agent.NewClient`
  accepts (no new module).
- AC: tests on Linux with injected OS facts: lock contention between two processes (re-exec); default dir per OS table;
  reserved names refused; `.gitattributes` rule test (a `\r` in any embedded script fails); CR-tolerant key, password and
  fingerprint readers; plain UI prints no `\033` when `Color=false`; `platform.SignalsToIgnore()` table; `GOOS=windows go vet` and
  `GOOS=darwin go vet` clean for the touched packages; behaviour on Linux unchanged (golden tests untouched).

### T11.3 - SD abstraction refactor (Linux behaviour unchanged)
- Files: `internal/sdcard/*` only (`facts.go`, `policy.go`, `inventory.go`, `opener.go`, `transport.go`, `platform*.go`,
  `blockio` in `internal/blockio/`, `inventory_linux.go`, `opener_linux.go`, `transport_unix.go`, edits to `backend.go`,
  `helper.go`, `sysfs.go` (removed/moved), tests), `cmd/akam/main.go` (sd-helper entry becomes
  `sdcard.HelperEntry(os.Args[1:], ...)` handling `sd-helper` and its per-OS flags).
- Deps: none (T11.2 has merged before: it only added the `platform.Init` call in `main.go`).
- Content: 4.4.1, 4.4.2, 4.4.3, 4.4.6, 4.4.7 (Linux column), 4.4.8 skeleton. `Backend` interface unchanged (`List`, `Check`,
  `NeedsElevation`, `Prepare`, `Wipe`) so `internal/cli` does not change. `newPlatform()` per-OS files return stubs that report
  "not supported on this build" for windows/darwin until T11.7/T11.8 replace them (so those tasks only **add** files).
- AC: every existing `internal/sdcard` and `internal/cli` SD test passes unmodified; new `Evaluate` table tests with Linux
  `Facts` fixtures reproduce every refusal of `guard_test.go`; `blockio.Aligned` test with an alignment-enforcing fake runs the
  full MBR + FAT32 writer and equals the direct write byte for byte; helper order-of-operations test with a recording `Opener`;
  the identity token string for Linux is byte-identical to today's.

### T11.4 - Backup location and private folders
- Files: `internal/backuploc/*` (new), `internal/cli/telnetcmds.go` (`bkPCDir`, file modes of SHA256SUMS/README/camera_info),
  `internal/cli/sdcmds.go` is **not** touched; state key `backup_root` (new, optional), tests.
- Deps: T11.2 (`fsutil.PrivateMkdir`, `platform.DocumentsDir`).
- Content: 3.2 and 4.7. Pure functions with injected facts: documents dir, cloud detection by path and environment, free space,
  Windows name rules, `-N` suffix on case-folded names.
- AC: table tests for Linux (with and without `user-dirs.dirs`), macOS, Windows (OneDrive redirected, plain, reserved names,
  trailing dot, long path), iCloud path; default suggestion per OS matches 3.2; the folder name never contains camera-supplied
  text; modes `0700/0600`; the existing `AKAM_BACKUP_ROOT` hook and all existing backup tests pass.

### T11.5 - Build, cross-compile, reproducibility, release pipeline
- Files: `tools/assistant/build.sh`, `Makefile`, `tests/assistant_test.sh`, `tools/build/deps.lock` (exact Go version and
  per-host sha256), `tools/build/bootstrap.sh` (plan text only), `tools/release/build_assistant.sh` (new),
  `tools/release/mkarchive/` (new small Go program, standard library only, deterministic tar/zip), `tools/release/README.md`.
- Deps: T11.1 (notices row), T11.2.
- Content: `build.sh all` builds the six targets into `bin/<os>-<arch>/akam[.exe]`; `build.sh sizes` per target with the table of 4.10 and
  `--by-package`; `build.sh repro` for **every** target (two builds each, same sha256); `build.sh vet-all` (`GOOS=<os> go vet`);
  test that `THIRD_PARTY_NOTICES.md` lists every module linked into each target
  (`go list -deps -f '{{with .Module}}{{.Path}} {{.Version}}{{end}}' ./cmd/akam` for each `GOOS/GOARCH`, so Windows-only
  modules such as `coninput` are covered); the release script of 8.1 (names, checksums, archives, notices, `SOURCE_DATE_EPOCH`);
  re-measure 2.3 and write the table to the task report; reproduce or explain the 14.6 MiB claim; evaluate the reductions of 4.10.
- AC: `tests/assistant_test.sh` cross-compiles all six and checks sizes against the budget; static check per OS where possible
  (`file`/`readelf` for ELF, `go version -m` for the others); two builds of each target identical; the pinned toolchain is enforced
  (a different `go version` is refused with a clear message unless `AKAM_ALLOW_ANY_GO=1`); script runs with no network.

### T11.6 - Camera options engine and missing commands
- Files: `internal/camopt/*` (new), `internal/cli/ctlcmds.go`, `internal/cli/helpercmds.go`, `internal/cli/optcmds.go` (new: `video`, `ntp`),
  `internal/cli/app.go` (dispatch lines only), `internal/cli/help.txt`, `internal/cli/remote.go` (the audited `restartVideo`,
  Q8), `internal/testfake/camera.go` (emulate `video.*_cfg`, `audio.mic_enable`, `akam_ntp.sh`), `testdata/gen_golden.sh`,
  `testdata/scenarios.txt`, `testdata/golden/*` (new scenarios), tests.
- Deps: none (parallel with T11.5, no shared file).
- Content: 4.5. Existing commands keep their exact output (golden). New commands match the bash wizard text and golden files
  (generated from `./akam`). The engine has no UI code: it returns structured `[]OptionState` and applies a `[]Change`.
- AC: golden parity for `video show`, `video bitrate 2000` (dry run), `ntp status`, `ntp set pool.ntp.org`, `audio mic off`; engine
  tests on the fake camera: read all groups in one exchange, apply a change set, report `Applies`/`Saved` correctly, validators
  reject out-of-range values and bad NTP names, an option missing on an old image reports `unavailable`, a camera-supplied value with
  control characters never reaches a command; `remote.go` test list still proves no `reboot|poweroff|halt|kill...` except the single
  named `restartVideo` whose exact command and default-No prompt are asserted.

### T11.7 - Windows SD disk layer
- Files: `internal/sdcard/inventory_windows.go`, `opener_windows.go`, `winparse.go` (untagged), `rawdev_windows.go`,
  `platform_windows.go` (replaces the stub of T11.3), `internal/sdcard/testdata/facts/windows/*`, `winparse_test.go`, `policy_windows_test.go`
  (untagged name, runs everywhere).
- Deps: T11.3.
- Content: 4.4.5 except the elevation transport; `rawDev`, lock/dismount, flush, update properties, uncached verify handle, constants
  for `FSCTL_*`/`IOCTL_*`.
- AC (Linux-runnable): parser tests on hand-built buffers (descriptor with each bus type, geometry, GPT and MBR layouts incl. LDM types,
  disk extents of a spanned volume); `Evaluate` matrix for Windows: system disk refused, boot disk refused, page-file disk refused,
  NVMe/SATA refused, USB stick under 128 GiB accepted with the red-box facts, USB disk over 128 GiB refused, dynamic disk refused,
  read-only refused, 4096-byte sectors refused, disk holding a guard path refused, drive letter given instead of a physical drive refused;
  recording-`Opener` order test (inspect, compare ident, lock all volumes, dismount, flush, verify, release); `GOOS=windows go vet` clean.
  The IOCTL calls themselves are covered by the hardware plan only (7.3).

### T11.8 - macOS SD disk layer and elevation
- Files: `internal/sdcard/inventory_darwin.go`, `opener_darwin.go`, `transport_darwin.go`, `plist.go` (untagged), `platform_darwin.go`,
  `internal/sdcard/testdata/facts/darwin/*`, `testdata/diskutil/*.xml`, tests.
- Deps: T11.3.
- Content: 4.4.4 including the sudo path and the opt-in `osascript` + Unix socket path (peer-credential check through
  `golang.org/x/sys/unix`), `diskN` to `rdiskN`, `diskutil` invocation through a fake-able `Runner` (so tests feed canned XML).
- AC (Linux-runnable): plist parser tests (dicts, arrays, booleans, integers, strings, nested APFS stores, malformed and
  oversized input refused, entity expansion not honoured); `Evaluate` matrix: the disk behind `/` refused by both checks, internal
  NVMe/Apple Fabric refused, APFS container with a system volume refused, USB card reader accepted, built-in SD slot reporting
  `Internal` accepted on protocol, disk image (`SystemImage`) refused, slice `diskNsM` refused, 4096-byte block refused; recording
  `Opener` order test (unmountDisk, recheck, open rdisk, write, fsync, verify); socket transport test on a Unix socket in a temp
  dir with a fake peer uid; `GOOS=darwin go vet` clean.

### T11.9 - Windows UAC transport
- Files: `internal/sdcard/transport_windows.go`, `pipe_windows.go`, `transport_win_test.go` (untagged fake-pipe tests), helper-side
  framing in `internal/sdcard/frame.go` (untagged, shared with macOS socket), edits to `HelperEntry` flag parsing in `internal/sdcard/entry.go`.
- Deps: T11.3 (T11.7 not required: the transport only needs `Transport` and `HelperEntry`).
- Content: 4.4.5 elevation. Pipe with first-instance flag, user-SID DACL, `PIPE_REJECT_REMOTE_CLIENTS`; `ShellExecuteExW` via LazyDLL with
  `runas`, `SW_HIDE`; server PID check; framing (request document, `{"progress"}` lines, final response); timeouts; decline mapping
  (`ERROR_CANCELLED`); already-elevated in-process path.
- AC (Linux-runnable): framing tests (oversize request, unknown field, second document, progress then final, truncated stream) shared
  with macOS; a fake `net.Pipe`-based transport proving one request one answer; cancellation message text; no secret appears in the
  launched command line (asserted on the argument builder); `GOOS=windows go vet` clean. Pipe ACL and UAC behaviour in 7.3.

### T11.10 - Discovery and known cameras
- Files: `internal/discover/*`, `internal/state/cameras.go` (new keys read/write helpers), `internal/cli/findcmd.go`,
  `internal/cli/cameracmds.go` (`cameras` listing, address-change reconcile), tests, `testfake` additions for port 8091 reads.
- Deps: T11.2 (state), T11.6 not required.
- Content: 4.6. `Iface` abstraction and per-OS name rules; per-interface multicast sender (`mcast_unix.go`, `mcast_windows.go`);
  `Identify` over 8091 read-only; `Run` orchestration; the camera table; known-camera freshness; address-change detection by host-key
  comparison; `ui.Sanitize` (shared with T11.13, created here in `internal/ui/sanitize.go`).
- AC: interface filter table tests with real-looking name lists for the three OSes (including Windows `vEthernet (WSL)` and macOS
  `utun3`); subnet selection excludes link-local and over-large ranges; `Identify` against the fake 8091 server returns version and
  host name, tolerates `ERR`, long lines, control characters and silence (2 s); an asserting fake proves only `GET` and `QUIT`
  words are ever sent; sanitiser tests (ESC sequences, bidi override, newline, overlong); known-camera merge (same address, moved
  address by host key, vanished camera shown as last seen); no network traffic before the user's yes in the scan flow (existing test style).

### T11.11 - Backup over SSH (Akam camera)
- Files: `internal/cli/backupssh.go` (new), `internal/cli/telnetcmds.go` (`cmdBackup` route choice, three lines), `internal/testfake/camera.go`
  (serve `dd if=/dev/mtdblockN`), tests.
- Deps: T11.4 (folder), T11.6 not required.
- Content: 3.7. Read-only: for each of the 8 partitions, stream `dd if=/dev/mtdblockN bs=64k` twice over the SSH session, compare
  size and sha256 between reads (CONFIG may differ: read again twice, existing rule), write the same folder format with `kind=akam`.
  Commands are fixed words only; the `remote.go` guard accepts `dd if=/dev/mtdblock` reads (read-only form) and still refuses `of=/dev/`.
- AC: fake-camera tests: success; a differing second read; a dropped session (no partial file left); size mismatch; folder contents and
  `SHA256SUMS` identical in format to the telnet route; the guard test proves no `of=` and no write path.

### T11.12 - squashfs reader
- Files: `internal/squashfs/*` (new, reader only), `internal/squashfs/testdata/`, tests.
- Deps: T11.1 (xz).
- Content: 4.8 step 1.
- AC: lists and extracts `tests/fixtures/app_fake*` and a synthetic image made with `mksquashfs` (test skips when absent; a few committed
  small images cover the no-tool case) identically to `unsquashfs -lls`/extraction; fuzz-style tests with truncated, corrupt and
  bomb-like images (declared sizes, inode loops, directory cycles, absurd counts) fail cleanly within the caps; no allocation driven
  by an unchecked field.

### T11.13 - Guided flows, home screen, licences
- Files: `internal/cli/misccmds.go` (menu), `internal/cli/steps.go`, `internal/cli/flows_*.go` (new), `internal/ui/ui.go` (+ widgets: `Form`,
  `Table`), `internal/ui/plain.go`, `internal/ui/tui/*` (+ golden files), `internal/cli/hostinfo.go` (OS-aware doctor),
  `internal/audio/capture.go` (dshow device selection), `internal/cli/licenses.go` (new) + `internal/assets` sync of `THIRD_PARTY_NOTICES.md`
  (embedded copy with a drift test), `internal/cli/app.go` (dispatch line for `licenses`), `internal/cli/help.txt` (one line).
- Deps: T11.3, T11.4, T11.6, T11.10, T11.11 (T11.7/T11.8/T11.9 only for the hardware plan).
- Content: section 3 in full on top of the engines: home, camera chooser, option screens, update, first install, backup/restore, settings;
  every flow written once against `ui.UI`; the existing step state machine keeps its keys.
- AC: tui golden files for each new screen (80x24 and a small window); plain-UI transcripts for each flow on the fake camera (first install
  to the flash on the fake stock camera and a file standing for the card; update; options edit and apply; backup route choice); every yes/no
  defaults to No; no destructive call without `flash.Confirmation` or the SD red box (existing safety tests extended to the new entry
  points); backup gate re-verifies `SHA256SUMS` before flash; `akam licenses` prints the embedded notices; doctor text per OS.

### T11.14 - squashfs writer
- Files: `internal/squashfs/writer*.go`, tests.
- Deps: T11.12.
- Content: 4.8 step 2, including the xz `CRC32` and 128 KiB dictionary validation.
- AC: output of the fixtures is readable by `unsquashfs` and by T11.12 (round trip), tree and modes equal to the input, size equal to a
  multiple of the block rules and <= 5242880 for the fake APP, the export table present, deterministic bytes for the same input and time;
  differences from `mksquashfs` output are listed in the task report (superblock fields compared field by field).

### T11.15 - Documentation, hardware plan, release notes
- Files: `docs/akam_v1_test_plan.md`, `docs/akam_v1_install.md`, `docs/akam_v1_flash_and_restore.md` (assistant on Windows/macOS), `README.md`
  (quick start), `tools/assistant/README.md`, `tests/readme_links_test.sh` stays green, a new `docs/assistant_install.md` (download, verify,
  Gatekeeper/SmartScreen steps from 8.3).
- Deps: T11.5, T11.13 (texts must match the screens).
- AC: links pass `tests/readme_links_test.sh`; the hardware plan of 7.3 is copied into `docs/akam_v1_test_plan.md` as tickable tables;
  8.3 workarounds present and checked against the released file names.

### T11.16 - Image assembly and the native `image` command
- Files: `internal/image/*` (new), `internal/cli/imagecmds.go` (new), `internal/cli/app.go` (dispatch: `build`/`image`), `internal/cli/help.txt`,
  `testdata/` goldens, tests.
- Deps: T11.12, T11.14, T11.13 (dispatch and flow wiring).
- Content: 4.8 step 3: port of the overlay copy, the two patches, safety checks, report, with the bash outputs as goldens on
  `tests/fixtures/app_fake*`; `--builder native|tools`; `--image FILE` import validates size 5242880 and records its md5.
- AC: image equal in tree/modes/hashes to the bash result on the fixtures (via the reader of T11.12 and `unsquashfs` when present);
  report fields equal; the default builder stays `tools` on Linux and "import" on Windows/macOS **until the gate of 4.8 step 4 is
  recorded**; a flag flips the default only in a later one-line change. The gate itself is a human task.

---

## 7. Test plan

### 7.1 Unit tests (Linux host, no network, no device)

| Area | What is tested |
|---|---|
| xz | the AC list of T11.1: valid, truncated, corrupt, multi-stream, multi-block, 4 GiB dictionary, 64 MiB dictionary, lying index, declared size over cap, memory ceiling; `kittest` builds kits with no external tool |
| Kit portability | symlink fallback and sidecar, no mode check on Windows, `IsLocal` on reserved names |
| Backup location | per-OS suggestion table, cloud detection, Windows name rules, free space, `-N` suffix, private modes, no camera text in names |
| Discovery | interface filter per OS, subnet size limits, `Identify` read-only word set, sanitiser, merge of known cameras, host-key address-change logic, no traffic before the yes |
| Options | validators, one-exchange read, change sets, `Applies`/`Saved` text, unavailable options, golden parity with bash for `video`, `ntp`, `audio mic`, no forbidden command except the audited video restart |
| SD guards (fakes per OS) | Linux: existing matrix through `Facts`. macOS: `/` disk, internal, Apple Fabric, APFS system container, disk image, slice, 4K block, USB reader ok, built-in SD slot ok, `Removable` unknown with `Internal` false ok. Windows: system, boot, page-file, NVMe, SATA, virtual, dynamic, read-only, 4K sectors, over-size, under-size, drive letter input, guard-path disk, USB stick ok, SD bus ok. Every OS: identity change between check and write refused; helper order of operations; closed operation set; request size and unknown-field refusal |
| SD I/O | `blockio.Aligned` against an alignment-enforcing fake with the real MBR + FAT32 writer; read-back mismatch detected; wipe spot-check |
| Transports | JSON framing shared by Windows pipe and macOS socket; Windows argument builder has no secret; macOS peer-credential check with a fake |
| State and platform | lock contention (two processes), default state dir per OS, reserved camera names, CR-tolerant readers, LF-only embedded scripts, atomic write retry |
| Console | no escape codes when colour is off; VT-failure fallback |
| Existing | golden files from bash, state round trip with the real `./akam`, fake camera and stock camera suites, safety tests (no reboot, confirmation bound to md5) |

### 7.2 Cross-compile, reproducibility, size (in `tests/assistant_test.sh` and `build.sh`)

- build all six targets with the release flags and the pinned toolchain; `GOOS=<os> go vet ./...` for the five non-host targets with
  `-tags akamtest` for vet only; `go test` runs for the host only (the Windows and macOS test binaries are compiled with
  `go test -c` for each target to catch compile errors in test files);
- sizes against 4.10 per target; `--by-package` report stored as a build log;
- reproducibility: two builds per target compared; the release script run twice on a clean checkout compared;
- notices completeness for every target (T11.5); embedded notices and scripts drift tests;
- repository rules already in the script: no symlink under `tools/assistant`, no private key, `go.mod` tracked. Add: no `//go:build`
  OS file contains the string `Evaluate(` (guard logic stays pure and shared).

### 7.3 Manual hardware test plan (what must pass before a platform is called ready)

Needed: a spare camera on supported hardware (stock, with the maintenance toolkit card route) and an Akam camera; a UART adapter at
hand for the flash tests; one microSD reader of each kind (built-in slot where the machine has one, USB reader) and one plain USB stick.
Record results in `docs/akam_v1_test_plan.md`. A platform is "ready" only when its whole column passes; otherwise the release marks it
"built, not hardware-tested".

**H1 - Windows 11 amd64** (repeat on windows/arm64 where hardware exists; Windows 10 1809+ spot check)

| # | Check | Pass |
|---|---|---|
| H1.1 | Download, SmartScreen "More info > Run anyway" per 8.3, `akam version`, `akam doctor` | opens, shows `windows/amd64`, no crash |
| H1.2 | Windows Terminal, classic console host and PowerShell: colours, box drawing, full-screen menu, arrow keys, resize, `q`, Ctrl-C | all render; Ctrl-C leaves a clean prompt |
| H1.3 | Password prompts (stream password, Wi-Fi password): no echo | no echo |
| H1.4 | State folder under `%LocalAppData%\Akam`; two instances at once | second refuses (lock) |
| H1.5 | SSH key creation in `%USERPROFILE%\.ssh`; ACL shows only the user, SYSTEM, Administrators; `ssh -i` (OpenSSH for Windows) works with the key | yes |
| H1.6 | ssh-agent (OpenSSH agent service) with a passphrase key | agent used |
| H1.7 | Camera discovery: TCP scan on Wi-Fi and on Ethernet; machine with WSL/Hyper-V/VPN adapters present; WS-Discovery with firewall prompt, both allow and block | cameras listed with host and firmware; virtual adapters not scanned |
| H1.8 | Backup folder suggestion (local Documents; OneDrive-redirected Documents; USB drive as target) | warns on OneDrive, ACL set, FAT/exFAT warns "not private" |
| H1.9 | `get-kit` on a clean profile (symlink-less extraction), then image import | kit verifies, no symlink error |
| H1.10 | SD guards: try the system disk, an internal NVMe, a USB stick, the card with an Explorer window open on it, a BitLocker To Go card, a write-protected card | system/NVMe refused; stick shows model and size; Explorer-open card fails with a clear drive-letter message before any write; BitLocker refused; write-protect reported |
| H1.11 | UAC: accept; decline (Cancel); run the whole program as Administrator | accept writes; decline writes nothing and says so; admin mode works with no prompt |
| H1.12 | Prepare backup card: result read back through the uncached handle; Windows Explorer shows `AKAMSD`; the stock camera boots it into telnet 24 | yes |
| H1.13 | Wipe, then prepare flash card with Wi-Fi password: `verify` passes; password not on disk elsewhere (`%TEMP%`, state, logs) | yes |
| H1.14 | Full first install of the spare camera from stock with an externally built image (until the native builder passes its gate), backup verified, flash, manual power cycle, `verify` | camera runs Akam |
| H1.15 | Update Akam to Akam over SSH; options edit and apply (LED, video, NTP, ONVIF) | applied, "applies at" texts true |
| H1.16 | Close the console window during an SD write and during the flash wait | card refused by the next verify; camera-side flash completes or aborts safely, message says not to power-cycle |
| H1.17 | Antivirus: Microsoft Defender with real-time protection on: no detection of the unsigned binary, or the documented exclusion/report steps work | recorded |
| H1.18 | Start to menu time (cold, Defender on) | under 100 ms of program time, recorded wall time |

**H2 - macOS arm64** (and amd64 on an Intel Mac, or the amd64 binary under Rosetta for everything except real Intel SD behaviour)

| # | Check | Pass |
|---|---|---|
| H2.1 | Download in a browser (quarantine) and with `curl`; Gatekeeper steps of 8.3; `akam version` | runs after the documented step; arm64 binary not killed |
| H2.2 | Terminal and iTerm2: colours, full-screen menu, Ctrl-C | ok |
| H2.3 | State folder `~/Library/Application Support/Akam`, key in `~/.ssh`, permissions 0700/0600 | yes |
| H2.4 | SD guards: internal SSD, a USB card reader, the built-in SDXC slot (if any; records whether `Internal` is true), a USB stick, a disk image mounted with Disk Utility | SSD and disk image refused; reader accepted; slot accepted on protocol; stick policy as designed |
| H2.5 | `sudo` elevation in a terminal (password prompt, request on stdin); opt-in `osascript` path | both write the card; decline writes nothing |
| H2.6 | Permissions: no Full Disk Access or Removable Volumes prompt for the raw write (record what appears) | recorded |
| H2.7 | After the write: automount of the new volume does not break verification; `diskutil eject` message correct | verify passes; extra `.fseventsd` harmless |
| H2.8 | iCloud Drive "Desktop & Documents" on: backup suggestion warns and proposes `~/Akam backups` | yes |
| H2.9 | Discovery with Wi-Fi and Ethernet both up; firewall prompt for WS-Discovery | listed; prompt text matches the note |
| H2.10 | First install from stock, update, options, restore, as H1.14/H1.15 | passes |
| H2.11 | `talk`/`say` with ffmpeg installed (`avfoundation`) and without (clear message) | ok |

**H3 - Linux regression** (amd64 and arm64, e.g. a Raspberry Pi): the existing matrix of spec 09 8.6 plus: refactored SD layer on a built-in
mmc slot and a USB reader (guard messages unchanged), `pkexec` and `sudo`, the new options screens, discovery, `backup --ssh` on an Akam
camera, native `image` once its gate is open.

**H4 - end to end per OS**: stock camera to verified Akam with only the binary (plus an externally built image on Windows/macOS until the
native builder gate), then restore to stock from the backup.

---

## 8. Release (R8)

### 8.1 Artifacts and names

Release tag `assistant-vX.Y.Z` (separate from kit tags, spec 09 10.1). Per release, files named without version:

```
akam-linux-amd64        akam-linux-arm64
akam-darwin-amd64       akam-darwin-arm64
akam-windows-amd64.exe  akam-windows-arm64.exe
akam-linux-amd64.tar.gz ... akam-windows-amd64.zip ...   (binary + LICENSE + THIRD_PARTY_NOTICES.md; keeps the exec bit on Unix)
THIRD_PARTY_NOTICES.md  LICENSE
SHA256SUMS              SHA256SUMS.sig                   (SSHSIG, namespace and principal akam-release, same scheme as the kit)
```

The raw binaries are what the brief asks for; the archives exist because a browser download of a raw Unix binary loses its
executable bit. `akam version` prints version, `GOOS/GOARCH`, Go version (existing). `akam licenses` prints the embedded notices so
that the notices also travel with a binary downloaded alone (BSD-3-Clause and MIT require the notice with binary
redistribution). `tools/release/build_assistant.sh` builds from a clean checkout on the reference host with the pinned toolchain,
`SOURCE_DATE_EPOCH` from the commit, writes `SHA256SUMS`, and `tools/release/sign_kit.sh`-style signing is run by the maintainer.
Reproducibility: the script is run twice and compared; a second machine's build is compared before the first public release.

### 8.2 Checksums and signature

Users verify with `sha256sum -c` (Linux), `shasum -a 256 -c` (macOS), `Get-FileHash -Algorithm SHA256` (Windows), and optionally
`ssh-keygen -Y verify`. `internal/kit/allowed_signers` is still a placeholder (2.2 row 29): the maintainer publishes the release public key in
the README and the release notes and adds the line **before** the first signed release (Q10).

### 8.3 Unsigned binaries (signing later)

Documented in `docs/assistant_install.md` (T11.15), summary:

- **Windows SmartScreen** ("Windows protected your PC"): More info, Run anyway; or right-click the file, Properties, tick Unblock; or
  `Unblock-File .\akam-windows-amd64.exe` in PowerShell. A UAC prompt for the SD step says "Unknown publisher" until the binary is signed.
  Defender may flag a new unsigned Go program: compare the SHA256 with the release, and report a false positive to Microsoft.
- **macOS Gatekeeper**: a file downloaded with a browser carries the quarantine attribute. Either download with `curl -L -o ...`
  (no quarantine), or `xattr -d com.apple.quarantine ./akam-darwin-arm64`, or Control-click Open once, or System Settings, Privacy &
  Security, "Allow Anyway". The arm64 binary carries the Go linker's ad-hoc signature, which the kernel requires; do not repack it.
  `chmod +x` is needed for a raw download.
- **Linux**: `chmod +x`.
- Later: Authenticode signing (Windows) and Developer ID signing plus notarisation (macOS) when the maintainer has certificates; the
  release script has a hook (`AKAM_SIGN_WINDOWS`, `AKAM_SIGN_MACOS`) that does nothing today, and signed artifacts get the same names.

---

## 9. Open questions (each with a recommended answer)

1. **Native image builder or something else (4.8).** Options: (a) own squashfs reader and writer plus the port of the build script, gated
   by a real flash; (b) static `mksquashfs`/`unsquashfs` per host in the kit; (c) interim only: Windows/macOS import an image built elsewhere.
   **Recommendation: (c) now, (a) as the planned track (T11.12 to T11.16) with the hard gate of 4.8 step 4; (b) only if the gate fails.**
   Windows/macOS first install is then honestly "needs an image from a Linux machine or WSL" until the gate passes.
2. **Windows card eligibility without the removable flag.** **Recommendation: accept bus USB, SD and MMC with the size gate and the
   system-role scan; show vendor, product, size and drive letters in the red box; do not require `RemovableMedia`.**
3. **Windows elevation transport.** **Recommendation: named pipe (4.4.5).** If the coordinator wants the request-file design from the
   brief: the file lives in a directory created with a protected DACL for the user and SYSTEM, the file name is a random hex, a 32-byte nonce
   goes on the helper's command line and the request carries `HMAC-SHA256(nonce, body)`, the parent waits on the helper's process handle, the
   helper deletes the file after reading and the parent deletes it again in a `defer`; the Wi-Fi password then touches the disk for a moment,
   which is why it is not recommended.
4. **State folders per OS.** **Recommendation: Linux unchanged (shared with the bash wizard); macOS `~/Library/Application Support/Akam`;
   Windows `%LocalAppData%\Akam`; no migration.** (The brief said `os.UserConfigDir`: on Windows that is the roaming `%AppData%`, wrong for
   host-key pins and caches.)
5. **Default backup folder.** **Recommendation: `<Documents>/Akam backups/<camera>/<date>` with the cloud-folder fallback and the privacy
   check of 3.2.** The bash and old Go default `~/akam-backup-*` stays accepted.
6. **Microphone (R5).** **Recommendation: feature-gated; keep external `ffmpeg`/`arecord`; fix the Windows dshow device selection; build a
   pure-Go Windows `waveIn` capture later only if users ask; no macOS pure-Go capture without authorising a module.**
7. **Wi-Fi editing.** There is no camera-side command to change Wi-Fi on a running Akam camera (only the flash-card route and
   `preflight_check.sh --ensure-wifi`). A wrong value locks the camera out of the network and only the card or UART recovers it.
   **Recommendation: read-only display now; editing needs a new camera-side helper (`akam_wifi.sh`, fixed-grammar, with a roll-back timer) and a
   kit rebuild, so it is a separate spec.**
8. **Video restart and the "no kill" rule.** The bash `video` offers `kill $(pidof ak_rtsp)` so the wrapper restarts the stream
   (`steps_camera.sh:1698-1703`); the Go guard forbids every `kill`. **Recommendation: allow exactly one audited function `restartVideo` with
   that exact command, behind a yes/no default No, never matching `watchdog`, tested; everything else stays forbidden.** Alternative: only
   say "applies at the next reboot" and never restart (simpler, less useful).
9. **Scan default.** **Recommendation: keep the scan and WS-Discovery questions at default No** (consistency with the safety rule, no
   traffic before a yes); the "Find cameras" menu entry shows the range so one `y` is quick.
10. **Release trust.** (a) Publish the release signing key before the first signed release; (b) add `akam verify-release FILE` to check another
    downloaded binary against `SHA256SUMS.sig` using the key built into the running binary (a few lines on top of `internal/kit/sshsig.go`);
    (c) add a GitHub Actions workflow that runs `go vet` and the unit tests on `windows-latest` and `macos-latest` (advisory, no hardware).
    **Recommendation: yes to (a) and (b); (c) yes, advisory only, since the repository decision so far is "no CI" (tools/release/README.md)
    and the coordinator owns that decision.**
11. **Budgets.** **Recommendation: 12 MiB hard, 11 MiB soft, for every target (4.10); find the cause of the reported 14.6 MiB before raising any
    number.**
12. **Minimum operating systems.** **Recommendation: Windows 10 1809+ (documented, Windows 11 tested), macOS as required by the pinned Go
    release (written by T11.5), nothing older promised.**
13. **Untested targets.** Hardware for windows/arm64 and darwin/amd64 may not be available. **Recommendation: still build and publish all six, label
    untested ones "built, not hardware-tested" in the release notes and the README table until a tester reports.**
14. **Camera name rules.** Rejecting Windows reserved names on every OS (T11.2) can in theory break an existing camera called `con`.
    **Recommendation: reject for new names only; an existing state directory with such a name keeps working on Linux and warns on Windows.**
15. **Stock camera discovery.** A stock camera is only found when it already runs the toolkit card. **Recommendation: say so in the screen, show the
    router-DHCP-list hint, and never probe more than the four ports; no OUI/MAC guessing.**

## 10. Decisions (coordinator, 2026-11-11)

Recommendations of section 9 are accepted except where noted.

- **Q1** (c) now: Windows and macOS import an image built on Linux or WSL; (a) native squashfs reader + writer + assembly port is the planned
  single code path (estimated 2500-3400 production LOC plus tests); (b) static squashfs-tools only if the (a) gate fails; (d) rejected.
  Corrections and additions from the advisory review:
  - The camera kernel is 4.4.302-cip94 (`tools/release/supported_hardware.txt`), not 3.4. Check its squashfs table sanity checks (id,
    lookup, xattr tables, table order and adjacency) against the 4.4.302 source before writing the writer.
  - Format: squashfs 4.0, xz, 128 KiB blocks, no BCJ, `-all-root -no-xattrs`, all times equal, fragments and export table present; xz
    streams with CRC32 (or none) and a dictionary <= 128 KiB (`ulikunitz/xz` defaults are CRC64 / 8 MiB and must be overridden).
  - The pipeline never unpacks to the host filesystem (reader -> in-memory tree -> patch -> writer); stock variants with BCJ, xattrs,
    hardlinks or sparse files are refused cleanly.
  - Gate before any real flash with the native builder: (1) reader matches `unsquashfs -lls` + extraction hashes on fixtures, mksquashfs
    images and a real dump; (2) writer superblock flags/table order match mksquashfs, canonical dumps equal; (3) differential edge-case
    trees (0 / 131072 / 131073 bytes, > 256 entries, metadata across 8 KiB); (4) every xz block decodes with system `xz`; (5) loop mount on
    the host kernel and on a vanilla 4.4.302 kernel in QEMU with a full read; (6) real dump through both builders, equal trees, deterministic
    native output; (7) loop mount read-only on an Akam camera if its kernel has loop support (no flash); (8) one flash on a spare camera with
    UART attached, recorded; (9) every user build re-reads its output and compares it with the plan before offering to flash.
- **Q2-Q9, Q11-Q15**: as recommended.
- **Q10**: (a) and (b) yes; (c) no GitHub Actions workflow for now (the repository decision is "no CI"; the maintainer decides later).
