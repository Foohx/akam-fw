# Akam - Specification: the assistant as a single binary (`akam`)

Status: proposal for review; language, TUI library and phases are decided by the user (section 0.2). Language of the tool: English (spec 02, DA2).
Nothing here was run against a camera. File:line references are against `dev` at 351ec26.

User request: "direct access to a binary for the assistant; the binary should be able to do almost everything by itself, e.g. built-in telnet connection etc."

---

## 0. Where we are today

### 0.1 Inventory of external tools

`./akam` (280 lines) dispatches ~6000 lines of bash in `tools/wizard/lib/*.sh`, plus `tools/sd`, `tools/flash_scripts`, `tools/release`, `tools/image`, `tools/build`. The wizard already avoids `telnet`, `nc`, `expect` on the PC (bash over `/dev/tcp`, `camsh.sh:2-3`), but needs the following:

| Tool | Where (file:line) | Purpose | Replacement |
|---|---|---|---|
| bash >= 3.2, coreutils (cat, cut, sed, awk, grep, head, tail, sort, find, xargs, tr, stat, id, mktemp) | everywhere; `camsh.sh:81`, `steps_camera.sh:520,1833`, `tui.sh:783` | glue | Go runtime |
| bash `/dev/tcp` | `camsh.sh` (whole), `camsh.sh:59-77` (`net_port_open`, `net_probe`), `transfer.sh:70-110` | telnet session, port probes, nc transfers | `net` (3.4, 3.5, 3.7) |
| `ssh` | `steps_camera.sh:78-86,703-706,947-950,1056-1062,1115,1274,1599` | commands, stream files, helpers | in-process SSH (3.2) |
| `ssh-keygen -t ed25519` | `steps_host.sh:1044,1047` | create `~/.ssh/akam` | in-process (3.2) |
| `ssh-keyscan`, `ssh-keygen -F/-lf` | `steps_camera.sh:91-95` | fingerprint, known_hosts lookup | in-process pinning (3.2) |
| `ssh-keygen -Y verify` / `-Y sign` | `verify_kit.sh:130-136`, `sign_kit.sh:7,28,33` | kit signature (namespace and principal `akam-release`) | in-process sshsig verify (3.8); signing stays maintainer-side |
| second Dropbear on 2222, `HostKeyAlias` | `reflash_over_ssh.sh:35-38`, `steps_camera.sh:79` | no-UART reflash | same alias logic (3.2, 3.6) |
| `nc` on the camera + `/dev/tcp` on PC | `transfer.sh:158,207`, `steps_camera.sh:467` | backup copy, image push over telnet route | in-binary dial + camera-side `nc -l` (3.5) |
| `nc -w 2 127.0.0.1 8091` on the camera | `steps_camera.sh:1266-1274,1352,1453,1544` | led/audio/picture control | SSH direct-tcpip to 127.0.0.1:8091 (3.3) |
| `timeout` | `transfer.sh:66` | deadline | context deadlines |
| `md5sum`, `sha256sum`, `shasum -a 256` | `host.sh:78-95`, `transfer.sh:35-45`, `steps_host.sh:814,854`, `steps_camera.sh:1985`, `verify_kit.sh:79-85,227` | hashes, `sha256sum -c` | `crypto/md5`, `crypto/sha256` (3.5) |
| `curl --proto =https --tlsv1.2 --retry 3` | `steps_host.sh:444-452,496,509` | kit + `SHA256SUMS(.sig)` | `net/http`, same constraints (3.8) |
| `tar -tvJf`, `tar -xJf`, `xz` | `verify_kit.sh:97,147-182` | list/extract kit | `archive/tar` + `ulikunitz/xz` (3.8) |
| `unsquashfs -s/-l/-lln/-d`, `mksquashfs -all-root` | `steps_host.sh:900-903,957`, `assemble_from_kit.sh`, `build_firmware.sh` | read stock APP, build image | native or optional tools (3.9) |
| `dd if=/dev/mtd7 bs=64k \| md5sum` on the camera | `steps_camera.sh:268,803`, `reflash_over_ssh.sh` | read-back verification | stays camera-side (section 4) |
| `git clone` toolkit `tasarren/lsc-tuya-toolkit` (pinned commit + tree hash) | `sd_card.sh:61,101`, `prepare_first_boot_sd.sh:30,116` | SD payload | HTTPS tarball + same tree hash (3.7) |
| `lsblk -P`, `findmnt`, `/sys` | `backend_linux.sh:6,46,110-133,212,243`, `prepare_first_boot_sd.sh:91-109,188-194` | removable-disk listing and guards | sysfs + mountinfo (3.7) |
| `wipefs`, `sfdisk`, `partprobe`, `blockdev`, `mkfs.vfat -F 32 -n AKAMSD`, `mount -o uid=,gid=,umask=022`, `umount`, `sync`, `dd bs=4M conv=fsync` | `backend_linux.sh:220-286`, `prepare_first_boot_sd.sh:197-229`, `sd_card.sh:444-582` | partition, format, fill, zero-fill | in-binary MBR + FAT32 on the raw device (3.7) |
| `sudo` (SD tool only) | `sd_card.sh:75`, `steps_host.sh:47-48,726,760` | disk access | `pkexec`/`sudo` re-exec of the binary, SD step only (3.7) |
| `ffmpeg` (`-f pulse/alsa/avfoundation`, decode to s16le 8 kHz) | `steps_camera.sh:1716-1727,1918-1919` | mic capture, file decode | **stays optional**, talk/say only (3.10) |
| `arecord`, `pactl info` | `steps_camera.sh:1716-1719` | mic capture | optional (3.10) |
| `setsid`, `mkfifo` | `steps_camera.sh:1833-1843` | capture process group | `os/exec` + pipe |
| `akam-talk` (C, `tools/akam-talk/akam_talk.c`) built with `gcc` or prebuilt | `steps_camera.sh:1653-1660` | ONVIF backchannel talk | ported to Go (3.10) |
| `stty` | `ui.sh:178-182`, `tui.sh:95,122-131` | raw keys, size | bubbletea / `x/term` (3.12) |
| `make`, `zig 0.14.1`, `gcc`, `git`, `bzip2`, `file`, `binutils`, ARM `gdb`, `shellcheck` | `steps_host.sh:581`, `deps.lock`, `check_host.sh` profiles | `./akam build`, CI | out of scope, stays bash/Zig (1.2) |
| `python3` | `tests/fixtures/pty_drive.py`, `tools/hw_tests/*.py` | tests/analysis only | unchanged |

Camera side the PC relies on: BusyBox `sh`, `dd`, `md5sum`, `nc`, `stat`, `cat` (stock firmware via telnet 24), and the Akam helpers `/usr/sbin/akam_hostname.sh`, `akam_rtsp_auth.sh`, `akam_debug.sh`, `akam_ssh.sh`, `akam_lockdown.sh` (`steps_camera.sh:1036,1104,1567`; `tools/release/image_files.txt`).

Related specs: 02 (wizard/kit/multi-OS), 03 (RTSP auth), 04 (control API 8091), 05 (audio out), 07 (standard protocols, ONVIF backchannel).

### 0.2 Decisions already taken (by the user)

- Language **Go**, static (`CGO_ENABLED=0`), SSH with `golang.org/x/crypto/ssh`, TUI with **bubbletea**.
- `ffmpeg` stays **optional**, only for `talk`/`say` (microphone capture, non-WAV decoding); everything else is in the binary.
- SD raw writes elevate with `sudo`/`pkexec` **only for that step**.
- **No auto-update.**
- The bash `./akam` keeps working during the transition.
- Phases: P1 core + ssh + daily commands; P2 backup/flash/update over SSH; P3 telnet + SD + first install from stock; P4 macOS/Windows.

---

## 1. Goals and non-goals

### 1.1 Goals
1. One static binary per OS/arch, no runtime dependency. Linux x86_64 and aarch64 first; macOS (arm64, amd64) and Windows (amd64) in P4, with the limits of 3.7 (raw SD access).
2. It does by itself: SSH (client, keys, host-key pinning), maintenance telnet, file transfer, hashing, find-camera, kit download + signature verification, backup, image build, flash (SSH and telnet), SD card preparation/verification/wipe, all daily commands, talk/say.
3. Same UX as the wizard: no argument = step menu with status (full-screen TUI on a terminal; `--plain` or non-tty = line-based); identical sub-command and flag names (`./akam help` today is the contract); English; every action shown before it runs; `--dry-run`; one yes/no before each state-changing step, default **No** (Enter, Esc, `q`); destructive actions (erase card, write flash) need the explicit yes in the red box and refuse without a terminal; **never flashes without explicit confirmation, never reboots or power-cycles the camera, never kills the camera watchdog** (no code path sends `reboot`, `poweroff`, `halt`, kills the watchdog or touches `/dev/watchdog`; enforced by a test, 8.1).
4. The bash wizard keeps working through the transition (1.3).
5. Measured budgets, opt-in features (Akam vision): size <= 12 MiB stripped (Linux), start to menu < 100 ms, no network unless a command needs it, no telemetry.

### 1.2 Non-goals
- Re-implementing `./akam build` (Zig 0.14.1, make, SmolRTSP, `deps.lock`): stays bash/Zig; normal users never compile (3.9).
- Changing what runs on the camera: helpers stay shell scripts (section 4).
- GUI/web UI, localisation, auto-update, other camera models (hardware gate `supported_hardware.txt` unchanged).
- Replacing ARM `flash_tool`/`dropbearmulti`/`ak_rtsp` (come from the kit).

### 1.3 Coexistence of the bash wizard and the binary
- Go module at **`tools/assistant/`** (module path `github.com/<owner>/camyn/tools/assistant`, `go.mod` there, vendored deps). It sits next to the other host tools and out of `src/` (firmware). Proposed alternative if the user prefers a top-level `cmd/`: not recommended, `tools/` is where every host tool already is and `tools/ci/local_check.sh` already walks it.
- Build output: `tools/assistant/bin/akam` (git-ignored). The root script `./akam` stays the bash wizard until P3 is accepted. **Release asset name is `akam`** (`akam-X.Y.Z-linux-amd64.tar.gz` containing `akam`), so users never see the collision: a user who downloaded a release has no script; a source checkout has the script at `./akam` and the Go binary at `tools/assistant/bin/akam`.
- Bridge: the bash `./akam` gets (in P1 acceptance) one line in `help`: "a native binary exists: see the releases" and **`AKAM_USE_BIN=1 ./akam ...`** execs `tools/assistant/bin/akam` (or `$AKAM_BIN`) with the same arguments, for A/B comparison. Default stays bash until P3 passes.
- After P3 acceptance and one release cycle: the root `./akam` becomes a 15-line launcher that execs the binary if found (PATH, `tools/assistant/bin`) else prints the download URL; the old bash dispatcher moves to `tools/wizard/akam-legacy` (still working "from source", still tested in CI) until a later decision (9.6).
- Both implementations use the same state files (section 5) and must not run at the same time.

---

## 2. Language: Go (decided), rationale

| Criterion | Go | Rust | Zig 0.14.1 |
|---|---|---|---|
| SSH client, known_hosts, ed25519, sshsig | `x/crypto/ssh` (+ `knownhosts`), mature | `russh`; or libssh2 (C) | none |
| Cross-compile from Arch | `GOOS=... CGO_ENABLED=0 go build`, 6 targets, no extra toolchain | possible, macOS awkward | excellent, but no libs |
| Size | 7-10 MB stripped | 3-6 MB | 1-3 MB |
| Stdlib for HTTPS, tar, hashes, test servers | complete | crates | thin |
| One-person maintainability | high | medium | low (pre-1.0, repo pins 0.14.1 for that reason) |

Licence: `LICENSE` is MIT (FluxAlchemist, 2026). Direct dependencies must be MIT/BSD/Apache-2.0: `golang.org/x/crypto`, `x/sys`, `x/term` (BSD-3), `github.com/ulikunitz/xz` (BSD-3), `charmbracelet/bubbletea`, `bubbles`, `lipgloss` (MIT), plus a squashfs library only if the 3.9 spike chooses one. GPL tools are only ever *executed* (never linked/embedded). All vendored, listed in `THIRD_PARTY_NOTICES.md`, Go toolchain pinned with sha256 in `tools/build/deps.lock` (like Zig). No cgo, hard rule. Per-OS code in `_linux.go`/`_darwin.go`/`_windows.go` with `x/sys`.

Layout under `tools/assistant/`:

```
cmd/akam/main.go          dispatcher, flags (same names as ./akam)
internal/cli/             one file per sub-command
internal/ui/              UI interface; plain implementation; tui/ (bubbletea)
internal/state/           state files, cameras, lock, migration
internal/camera/ssh/      client, keys, known_hosts, exec, forward, put/get
internal/camera/telnet/   maintenance telnet (camsh port)
internal/camera/xfer/     nc-based and SSH transfers, hashes
internal/camera/ctl/      control port 8091 client
internal/discover/        probe, subnet scan, WS-Discovery, name lookup
internal/kit/             download, sshsig verify, tar/xz safe extract
internal/image/           stock APP reader, builder, patches, report
internal/sdcard/          enumeration, guards, helper protocol, MBR, FAT32, per-OS backends
internal/rtsp/            RTSP + Digest, ONVIF backchannel, G.711, WAV
internal/audio/           optional capture backends (external processes)
internal/testfake/        fake camera (SSH, telnet, 8091, RTSP) for tests
```

---

## 3. Built-in replacements

### 3.1 Commands and process model
`akam <command>` mirrors `./akam help`: `doctor`, `get-kit`, `build` (delegates, 3.9), `prepare-backup-sd`, `find-camera`, `backup`, `image`, `prepare-flash-sd`, `flash`, `verify`, `update`, `hostname`, `rtsp-auth`, `led`, `audio`, `picture`, `debug`, `talk`, `say`, `restore`, `wipe-sd`, `step`, `status`, `cameras`, `rename-camera`, `register`, `help`, with all options (`--dry-run`, `--kit`, `--version`, `--allow-unsigned`, `--expect-sha256`, `--backup-dir`, `--backup-copy`, `--ip`, `--identity`, `--image`, `--manual-telnet`, `--out`, `--comp`, `--device`, `--pubkey`, `--ssid`, `--profile`, `--camera`, `--state-dir`, `--no-color`, `--plain`). New: `telnet`, `onvif` (when the wizard integration of `feature/onvif-wizard` lands), `self-test`, `version`, `verify-self`, `forget-host-key`, hidden `sd-helper`. `doctor` now reports version, kit presence, optional helpers found (`ffmpeg`, recorders, `mksquashfs`), ability to elevate for the SD step. Exit codes unchanged (0 ok, 1 failure, 2 usage).

### 3.2 SSH client, keys, host-key pinning
- `x/crypto/ssh`, user `root`, port 22 (`AKAM_PORT_SSH`). Host-key algorithms limited to what `akam_ssh.sh` generates (to confirm, Q3).
- Key generation: ed25519 in OpenSSH format (`ssh.MarshalPrivateKey`) at `~/.ssh/akam` (0600) + `.pub`, comment `akam`; `--identity`/`--pubkey` kept; optional passphrase prompted without echo; `SSH_AUTH_SOCK` agent supported (Windows named pipe in P4). Only `ssh-ed25519` public keys go on the card (`steps_host.sh:1002`); a private key is refused (`steps_host.sh:997`).
- Pinning: dedicated `~/.local/state/akam/known_hosts` (OpenSSH line format, `HostKeyAlias`=IP convention so one pin also covers the RAM Dropbear on 2222) via `knownhosts`. First contact shows the SHA256 fingerprint and asks yes/no (default No); non-tty refuses unless `--trust-new-host-key FP`. **A changed key is a hard error**; only `akam forget-host-key IP` (explicit, logged) clears it. The user's `~/.ssh/config` is ignored.
- Primitives: `Exec`, `ExecStdin` (secrets on stdin, as `steps_camera.sh:1115`), `Forward`, `Put`/`Get` via `cat` on an exec channel (no sftp/scp on Dropbear), always followed by size + hash check.
- Connect timeout 10 s, keepalive 15 s, per-command deadlines (flash read-back 600 s). Log lines `CMD ssh root@IP <cmd>` only for commands carrying no secret.

### 3.3 Control port (led, audio, picture, debug)
Today: `printf 'GET/SET...' | nc -w 2 127.0.0.1 8091` over SSH. In-binary: SSH `direct-tcpip` to `127.0.0.1:8091`, same GET/SET lines, same parsing (drop `LOG ` lines), same ranges (picture -50..50, volume 0-100, debug minutes 1-1440, default 60). Loopback trust model unchanged. `debug on` keeps its extra yes/no (default No) and runs `akam_debug.sh` over SSH (`steps_camera.sh:1567,1599`).

### 3.4 Telnet client (stock maintenance telnet, port 24)
Port of `camsh.sh` into `internal/camera/telnet`: PC dials out only; IAC negotiation parsed and refused; CR and ANSI (`ESC[6n`, BusyBox line-editor echo and 80-column wrapping) stripped; one command per send, <= 350 bytes, wrapped with nonce markers built by the shell (`__AKAM_BEG_<nonce>_<n>_` / `__AKAM_DONE_<nonce>_<n>:<rc>__`), output before BEG dropped, inactivity timeout 30 s (`AKAM_CAMSH_TIMEOUT`). After 2 consecutive failures or `--manual-telnet`: **manual mode** (print the command; user pastes it in their own terminal; the tool never invents output). The `YES` to the camera flash script is never sent by code, only what a human typed (`camsh.sh:25-26`). `akam telnet [IP]` is an interactive passthrough (`Ctrl-]` quits). Stock+toolkit vs Akam detection by port probe as `steps_camera.sh:146-178`.

### 3.5 File transfer and hashing
Stock firmware has only BusyBox and a telnet shell, and the PC firewall blocks inbound, so the existing pattern is kept in-process:
- Over telnet: Get = camera `nc -l -p PORT < file` (via the shell), PC dials and reads; Put = camera `nc -l -p PORT > file`, PC dials and writes. Size (`stat -c%s`) and hash (`sha256sum`, else `md5sum`, probed as `transfer.sh:35`) are taken on the camera, the PC hashes while streaming. 3 tries (`AKAM_XFER_TRIES`), settle delay and session check as `transfer.sh:110-130`; temp file renamed only after verification.
- Over SSH: `Put`/`Get` of 3.2.
- Backup (`steps_camera.sh:290-447`): `dd` the 8 MTD partitions twice on the camera, compare, copy dump1, verify; output `~/akam-backup-<name>-<date>/` with `mtdblock0..7.bin` and `SHA256SUMS` exactly as today (`sha256sum -c` keeps working, bash wizard accepts the folder).

### 3.6 Flash and restore
Same semantics, ported control flow.
- SSH route (`reflash_over_ssh.sh`): upload image + `flash_tool` + camera scripts to RAM (`/tmp/akam_reflash`), camera starts a second Dropbear on 2222 from RAM (pinned via the same alias), flash started fully detached, log polled on 2222, read-back md5 of `/dev/mtd7` verified, then the user is told to power-cycle **by hand**. Image exactly 5242880 bytes, md5 checked locally and on the camera; the no-UART risk is spelled out and needs the typed/clicked yes.
- Telnet + card route (`steps_camera.sh:715-830`): exactly one image in `/mnt/custom_firmware` with the same md5, files pushed only after the yes, script prompt relayed, read-back `dd if=/dev/mtd7 bs=64k | md5sum`.
- Gates kept: camera-side `flash_guard.sh`, `preflight_check.sh`; verified backup; image not stale (`steps_host.sh:254-311`); hardware in `supported_hardware.txt`. No reboot is ever sent; after a failed write: "DO NOT power-cycle" text.

### 3.7 find-camera and SD cards
**find-camera**, each level opt-in (yes/no before any scan; typing an IP stays the default):
1. probe a given IP (parallel dial, 1.5 s) and classify (stock+toolkit / Akam / unknown);
2. resolve the camera name (`net.LookupHost`, mDNS `<name>.local`);
3. ONVIF WS-Discovery Probe (UDP multicast 239.255.255.250:3702) answered by Akam cameras (spec 07);
4. subnet scan: local /24 of each non-virtual interface, the four camera ports only (24, 22, 554, 8091), <= 64 concurrent dials, 2 s timeout, range shown before. Only this finds a stock camera.

**SD cards** (the privileged, hardest part):
- *Enumeration (no privilege)*: Linux `/sys/block/*` (`removable`, `size`, `device/model`, transport) and `/proc/self/mountinfo`. Rules identical to `backend_linux.sh`: whole removable USB disks or `mmcblk` readers, 128 MiB..128 GiB; refuse partitions, SATA/NVMe/loop, any disk carrying `/`, `/boot`, `/home`, `/var`, swap (all mounts incl. subvolumes), the repo, `$HOME`, cwd, temp dir, LUKS/LVM/RAID members or holders; `/dev/disk/by-*` resolved first; identity (size, model, serial, wwn, PTUUID) recorded.
- *Elevation (only for this step)*: after the red-box confirmation the process announces "This step needs administrator rights to write the card. Continue? [y/N]" and re-executes itself as `pkexec <self> sd-helper` (or `sudo` when no polkit agent/display or `--sudo`), request as one JSON line on stdin (never argv): `{op, device, identity, mode, payload}`. The rest of the program never runs as root and never runs sudo silently.
- *Helper (closed operation set)*: `list`, `prepare-backup`, `prepare-first-boot`, `add-flash-files`, `verify`, `wipe`. It re-checks identity and eligibility right before each destructive call, opens the device `O_RDWR|O_EXCL`, unmounts with `umount2`, writes an MBR (one active FAT32-LBA partition, as `sfdisk 'label: dos\n, , c, *'`), formats FAT32 (label `AKAMSD`) with an in-binary writer (boot sector, FSInfo, 2 FATs, LFN, cluster size by card size), writes the files **directly on the raw device** (no mount, no `uid=/umask` options, no automounter race), `fsync`, `BLKRRPART`, then re-reads the volume with a read-only FAT32 reader to verify (flags, hooks, hashes against the toolkit tree, sizes, no `*.bin` at root: the checks of `sd_card.sh verify`). `wipe` zero-fills exactly the device size (4 MiB blocks, fsync). No network and no downloads inside the helper.
- *Wi-Fi password*: asked unprivileged without echo (or `AKAM_WIFI_PASS`, removed from the environment at start, `akam:155-158`), sent to the helper only over the pipe, written clear text to `custom/configs/wifi.conf` as today (the card is a secret; `wipe-sd`), never logged.
- *Toolkit payload*: `tasarren/lsc-tuya-toolkit` at the pinned commit; without `git`, download the GitHub tarball for that commit over HTTPS and check the same C-locale tree hash (`sd_card.sh:60-79`); refuse on mismatch (Q5: embed instead).
- *Per OS*:
  - Linux: as above (`pkexec` or `sudo`; no elevation when the user already has write access to the device).
  - macOS (hard): `/dev/rdiskN` needs root, Disk Arbitration auto-mounts (run `diskutil unmountDisk` first), TCC can deny removable-volume access to a terminal, no `pkexec` (`sudo` in the terminal, or `osascript ... with administrator privileges`). Feasible with the same MBR+FAT32 writer. Fallback: `diskutil eraseDisk FAT32 AKAMSD MBR diskN`, then the binary writes files on the mounted volume without root.
  - Windows (hardest): `\\.\PhysicalDriveN` needs Administrator (UAC re-launch), volumes must be locked and dismounted (`FSCTL_LOCK_VOLUME`, `FSCTL_DISMOUNT_VOLUME`), writing the MBR of a mounted disk needs `FSCTL_ALLOW_EXTENDED_DASD_IO`, `IOCTL_DISK_UPDATE_PROPERTIES` afterwards, bus-type guards via `IOCTL_STORAGE_QUERY_PROPERTY`. Needs a Windows test machine. First Windows release may be **files-only** (user formats FAT32 with the OS, the binary writes onto the mounted volume; `verify --mount` already exists).

### 3.8 Kit download and signature verification
- HTTPS only, redirects only to HTTPS, TLS >= 1.2, retries 3, resumable into `~/.local/state/akam/downloads`; files `akam-kit-<version>.tar.xz`, `SHA256SUMS`, `SHA256SUMS.sig` from `https://github.com/$KIT_REPO/releases/download/v<version>/` (`steps_host.sh:478-509`); `--kit FILE|DIR`, `--version`, `--expect-sha256` (>= 8 hex digits), `--allow-unsigned`, `--allow-test-kit` keep their meaning.
- **Scheme kept unchanged: OpenSSH SSHSIG** (`ssh-keygen -Y sign -n akam-release` over `SHA256SUMS`; verify with principal `akam-release`, namespace `akam-release`, `verify_kit.sh:130-136`, `sign_kit.sh:28`), ed25519. The binary parses the armored `-----BEGIN SSH SIGNATURE-----`, checks magic/version/namespace/hash algorithm (sha256 or sha512), verifies `"SSHSIG" || namespace || reserved || hash_alg || H(message)` with `ssh.PublicKey.Verify`, and parses `allowed_signers` (principal, `namespaces=`, key). Cross-tested in CI against `ssh-keygen -Y` both ways.
- `tools/release/allowed_signers` is embedded (`go:embed`) and overridable with `--allowed-signers`. It is a placeholder with no key today (`allowed_signers:1-4`): behaviour kept (signed kit refused until a key is published; unsigned needs `--allow-unsigned`, loud warning).
- Archive rules exactly as `verify_kit.sh:6-25`: copy tarball to a private dir first; list first (regular files, directories, the two dropbear symlinks; names `[A-Za-z0-9._+/-]`; no absolute/`..`; no duplicates; sane modes; size cap); extract with `archive/tar` + `ulikunitz/xz` into a private dir (no overwrite, no owner); check `SHA256SUMS` and `MANIFEST.txt` for every file (content, mode, symlink target), `kit_format` in `kit_compat.txt`, `time64_gate=pass`, `kit_grade=release`, `supported_hardware.txt` present. **Nothing from the kit is executed on the PC.**
- The binary cannot verify itself: binary releases are signed with the same key (7.2); `akam verify-self SHA256SUMS SHA256SUMS.sig` exists for use from an already trusted binary.

### 3.9 Image build (normal users never compile)
Today `assemble_from_kit.sh`: `unsquashfs` the stock APP (`mtdblock7.bin`), `build_firmware.sh --kit` overlays Akam files (list `tools/release/image_files.txt`), applies `patch_service_sh.sh` and `patch_ethernet_hostname.sh`, builds `mksquashfs -all-root` with the **same compression as the stock APP** (xz/gzip), checks, pads to 5242880, writes `build_report.txt`, compares the dump to `supported_hardware.txt`. All binaries and overlay files come from the signed kit, so **no compiler is needed**; a squashfs reader and writer are.

Design:
1. `internal/image` reads the stock squashfs 4.0 (xz/gzip, fragments, xattrs ignored), applies the kit overlay with manifest modes, applies the two patches, writes squashfs with the stock compressor/block size, `-all-root`, pads to 5242880, then re-reads its own output and compares tree + hashes to the plan.
2. Patches: Go text transforms with the same anchors and refusal messages; the kit keeps the shell versions as reference; a CI test checks both give identical results on `tests/fixtures/app_fake*`.
3. Device nodes: written directly as inodes, so no root/`fakeroot` (the bash path refuses without root).
4. **Decision gate (risk).** A Go squashfs writer that the camera kernel mounts is the one big unknown. A one-week spike evaluates `diskfs/go-diskfs`, `CalebQ42/squashfs` or an own ~1500-line writer for the narrow subset (no xattrs, no export table, one compressor). Acceptance: `unsquashfs -s/-l/-d` and a loop `mount -t squashfs` in CI accept the image, file list/modes/hashes equal the `mksquashfs` image, then **one real flash** on the maintainer's camera. Until then `image` uses `unsquashfs`/`mksquashfs` when found (reported by `doctor`, install command printed as today) and `--builder native` opts into the Go builder. Fallback if the spike fails: the kit ships static `mksquashfs`/`unsquashfs` per host OS/arch (GPL-2.0, separate executables, source offer), run from the verified kit.
5. Gates as today: verified backup, hardware verdict, time64 gate, compression match; outputs `ak_rtsp_firmware_*.squashfs`, `.sha256`, `build_report.txt` in `~/akam-image-<name>-<date>`; stock files only in the temp dir and `app_patched/`, never published.
6. `akam build` (compile from source) is not reimplemented: from a source checkout it delegates to `tools/build/*` and `src/ak_rtsp/*`; otherwise it explains.

### 3.10 Audio: talk and say
- RTSP + ONVIF backchannel + Digest in `internal/rtsp`, port of `tools/akam-talk/akam_talk.c`: one TCP connection, DESCRIBE with `Require: www.onvif.org/ver20/backchannel`, Digest (MD5, `qop=auth`, nonce counting), SETUP send-only audio with interleaved RTP, PLAY, RTP every 20 ms, RTCP BYE, TEARDOWN. Exit codes as the C client (3 camera refuses talk, 4 login refused, 5 protocol, 6 no audio). Password: prompt without echo, `AKAM_RTSP_PASSWORD`, or `--password-file`; never argv, never logged. The Digest client also serves `rtsp-auth status` and `verify` probes (401 when auth is on, spec 03); no RTP receive path.
- G.711 (A-law/mu-law per SDP) in Go, test vectors from `tests/native/g711_test.c` and `talk_test.c`.
- Input: WAV read in-binary (mono mix and linear resampling to 8 kHz); `-` reads raw s16le from stdin. `talk test` (2 s tone) and `say file.wav` need **no external tool**.
- **ffmpeg stays optional, only here**: `say` for non-WAV formats (`steps_camera.sh:1918`) and `talk` microphone capture, because OS audio needs ALSA/Pulse/PipeWire, CoreAudio or WASAPI and there is no cgo-free path worth depending on. Auto-detected, spawned with `os/exec` and read from a pipe (no `mkfifo`/`setsid`): Linux `ffmpeg -f pulse` when Pulse/PipeWire answers, else `arecord -q -t raw -f S16_LE -r 8000 -c 1`, else `ffmpeg -f alsa` (existing order, `steps_camera.sh:1716-1723`); also `pw-record`/`parec` when present; macOS `ffmpeg -f avfoundation -i ":default"`; Windows `ffmpeg -f dshow`; `AKAM_TALK_CAPTURE` override kept. Missing tool: a message with the distro install command, nothing else is affected. Ctrl-C sends TEARDOWN.

### 3.11 TUI and plain mode
- **bubbletea** (with bubbles/lipgloss) for the full-screen menu, same behaviour as `tui.sh`/`tui_menu.sh`: steps with status (done/todo/blocked/stale), arrows or `j/k`, Enter, `?`, `q`; widgets yes/no (default No), red danger box, progress, spinner task, text prompt, hidden prompt, picker.
- Boundary: an `ui.UI` interface (`Say`, `Warn`, `Fail`, `AskYN`, `Pick`, `Secret`, `Progress`, `Danger`); commands never import bubbletea, so the plain implementation and tests are independent of it. bubbletea is a size cost (~2-3 MB) counted in the budget.
- `--plain` / no tty / `AKAM_PLAIN=1`: line-based with the same strings as the bash wizard; `--no-color`/`NO_COLOR` honoured; Windows: enable VT processing, fall back to plain on old consoles.

### 3.12 Self-test
`akam self-test` runs offline embedded checks (hashes, embedded sshsig sample, G.711 vectors, FAT32 writer into memory + re-read, tar safety rules, telnet parser on recorded transcripts). `--log` keeps `akam.log` with secrets redacted.

---

## 4. Camera side versus binary

| Stays camera-side (image or card) | Why |
|---|---|
| `akam_ssh.sh`, `akam_lockdown.sh` | boot-time behaviour, no PC present |
| `akam_hostname.sh`, `akam_rtsp_auth.sh` (hash + credential files, `set-creds`, `status`, `disable`), `akam_debug.sh` | own their files and validation; the PC invokes them, password on stdin |
| `ak_rtsp_wrapper.sh`, `_ht_process.ini`, `service.sh`/`ethernet.sh` patches | boot flow |
| ARM `flash_tool`, `flash_guard.sh`, `preflight_check.sh`, `install_with_flash_tool.sh`, `restore.sh`, `akam_hwcheck.sh` | only the camera can write/verify `/dev/mtd7` and its own ports |
| `dd`/`md5sum` read-back, 8091 control server, RTSP/ONVIF servers | firmware / ground truth |

| Moves into the binary | Replaces |
|---|---|
| orchestration, state, gates, confirmations, menu | `akam` + `tools/wizard/lib/*.sh` |
| SSH, telnet, transfer, hashing, control-port client | `ssh`, `nc`, `/dev/tcp`, `md5sum`, `sha256sum` |
| kit download/verify/extract, image assembly (3.9 gate) | `curl`, `tar`, `xz`, `ssh-keygen -Y`, `unsquashfs`, `mksquashfs` |
| SD tool | `lsblk`, `sfdisk`, `mkfs.vfat`, `mount`, `dd`, `git`, `sd_card.sh` |
| talk/say client, G.711, WAV | `akam-talk` (C) |

Principle: the camera keeps every decision that protects the camera (flash guards, lockdown); the PC keeps every decision that protects the PC (disk eligibility, kit verification, nothing downloaded is executed). Camera helper interfaces (arguments, exit codes, output lines like `not ONVIF-ready`, `invalid`) are a versioned contract; the binary probes helper capability and refuses politely against an older image (as `hostname` already says it needs `akam_hostname.sh`).

---

## 5. State and compatibility

- Layout unchanged: `~/.local/state/akam` (`$XDG_STATE_HOME`, `AKAM_STATE_DIR`, `--state-dir`): `state` (+`state.dry`), `akam.log`, `current_camera`, `kit/`, `downloads/`, `cameras/<name>/{state,akam.log,reflash logs}`; camera "default" at the base dir (`cameras.sh:4-13`). Same `KEY=VALUE` format (`[a-z0-9_]+` keys, one line, **no secrets**, 0600, atomic replace, `state.sh:38-46`).
- Bash and Go read and write the same files, so a user can alternate during the transition. The binary takes a lock (`flock`/`LockFileEx`); bash is unchanged, so "do not run both at once" is documented.
- New keys are prefixed `bin_`, ignored by bash (tested by a round-trip). The binary also writes `tool=go`, `tool_version`; `wizard_version` untouched.
- Migration is explicit: (1) existing `state` opened as is, one copy `state.bak-before-binary`; (2) offer to import `~/.ssh/known_hosts` entries aliased to registered camera IPs into the dedicated file (fingerprints shown, yes/no); (3) `~/.ssh/akam` reused; (4) the same `kit_path`/`kit_sha256`/`image_path`/`image_sha256` keys keep step validation valid across implementations.
- Folder and file names unchanged (`~/akam-backup-<name>-<date>`, `~/akam-image-<name>-<date>`, `mtdblock0..7.bin`, `SHA256SUMS`, `ak_rtsp_firmware_*.squashfs`, `build_report.txt`). Kit formats: `accept_kit_format|1`; a newer kit needs a newer binary.

---

## 6. Security

1. **Secrets never logged**: Wi-Fi password, stream password, SSH passphrase, ONVIF credentials are read without echo (`x/term.ReadPassword`), kept in memory only as needed, never on a command line, never in a child's environment (read once and removed, as `akam:155-158`), never in state files; all log output passes a redaction list; a test greps every command's log for the test secrets. No raw-I/O debug mode that prints password exchanges.
2. **Host-key pinning**: dedicated file, fingerprint confirmation on first use, hard failure on change, no `StrictHostKeyChecking=no` equivalent anywhere.
3. **Signed kit**: 3.8; unsigned only with `--allow-unsigned` (loud warning), test kits only with `--allow-test-kit`, `--expect-sha256` cross-check; binary releases signed with the same scheme.
4. **Safe defaults**: yes/no default No before each state-changing step; red box for destructive ones; refusal without a terminal; `--dry-run` changes nothing; no reboot/power-cycle/watchdog action ever (test, 8.1); flash needs verified backup + md5 match + hardware gate + explicit yes; transfers verified by size and hash before use.
5. **Privilege separation for SD writes**: the main process is never root; only `akam sd-helper` is elevated, by a user-visible prompt naming device and operation; closed operation set; input on stdin only; the helper re-validates every guard independently of its caller and re-checks identity before each destructive call; no network; exits after one operation; no SUID, nothing installed system-wide.
6. **Untrusted input**: kit archives (3.8), camera output (marker-bounded, length-capped, ANSI stripped), toolkit tarball (tree hash), HTTP (size caps, HTTPS-only redirects); camera/kit paths pass `filepath.IsLocal` before any host path is built.
7. **Network exposure**: no listening socket (PC always dials the camera); scans and WS-Discovery only when chosen.
8. **Supply chain**: vendored pinned modules, `go.sum`, reproducible build, signed release, no network in `go:generate`.
9. **Temp files**: `os.MkdirTemp` mode 0700 under the state dir, removed on exit and on signals; stock APP contents only in temp and `app_patched/`.

---

## 7. Distribution

1. Release artifacts: `akam-X.Y.Z-linux-amd64`, `-linux-arm64` (P1); `-darwin-arm64`, `-darwin-amd64`, `-windows-amd64.exe` (P4); archives (`.tar.gz`, `.zip` on Windows) with the binary, `LICENSE`, `THIRD_PARTY_NOTICES.md`; plus `SHA256SUMS` and `SHA256SUMS.sig` (SSHSIG, namespace `akam-release`, same key as the kits, one `allowed_signers` line).
2. Verification: download, `sha256sum -c`, optionally `ssh-keygen -Y verify` or `akam verify-self`; the public key is quoted in README and release notes.
3. Reproducible build: pinned Go (`deps.lock`, sha256), `-trimpath -buildvcs=false -ldflags "-s -w -X main.version=..."`, `CGO_ENABLED=0`, vendored modules, fixed `SOURCE_DATE_EPOCH` for archives; `tools/release/build_assistant.sh` builds all targets from a clean checkout on the Arch x86_64 reference host; a CI job rebuilds and compares hashes. macOS/Windows binaries unsigned at first (Gatekeeper/SmartScreen steps documented, Q4).
4. Size budget (CI fails over it): Linux binary <= 12 MiB stripped, archive <= 5 MiB, each dependency reports its delta; start to menu < 100 ms (`help` < 50 ms); idle RSS < 40 MiB; `talk` < 5 % of one core.
5. **No auto-update.** The tool contacts the network only for what the user asked (`get-kit`, `find-camera`, toolkit download). Updating is manual: download, verify, replace. Optional `akam version --check` (one HTTPS GET, no identifiers) only when the user runs it. A tool that writes flash must not change code unattended.
6. Docs: README quick start becomes "download `akam`, run it" at P3 acceptance; `docs/akam_v1_install.md` keeps the bash path as "from source"; `tests/readme_links_test.sh` stays green.

---

## 8. Testing strategy

### 8.1 Unit tests (`go test ./...`, no network, no device)
- Parsers/encoders: telnet IAC and marker extraction, state file, `allowed_signers`, SSHSIG (valid + wrong namespace/key/tampered), every tar refusal of `verify_kit.sh`, SHA256SUMS, MBR/FAT32 writer, G.711, Digest (RFC vectors), SDP, control-port lines.
- Safety tests: (a) command allow-list: camera commands come from a few constructors and none contains `reboot`, `poweroff`, `halt`, watchdog kill/`/dev/watchdog`, `init 6`; (b) secret redaction in all logs; (c) disk-eligibility matrix from fake sysfs/mountinfo trees in `testdata/` (same cases as the bash backend tests); (d) default-No for every prompt (table test, `--plain`, scripted stdin).
- Parity: for each bash test covering wizard behaviour (`kit_test.sh`, `flash_guard_test.sh`, `reflash_ssh_test.sh`, `lockdown_test.sh`, `hostname_test.sh`, `cred_test.sh`, `first_boot_sd_test.sh`, `akam_debug_test.sh`, `akam_talk_test.sh`, `build_from_kit_test.sh`) a Go test with the same fixtures (`tests/fixtures/app_fake*`), messages and exit codes.

### 8.2 Fake camera
`internal/testfake` (also built as `akam-fakecam` for manual runs, replacing `tools/wizard/fake_camera.sh`):
- SSH server (`x/crypto/ssh` server side, ed25519 host key) that runs the **real helper scripts** from `tools/firmware_patch_templates/` in a temp rootfs (as `tests/fixtures/fake_camera_env.sh`); exec, stdin, `direct-tcpip` to a fake 8091, `cat >`/`cat <`, and a second server standing for port 2222;
- telnet-port server with a BusyBox-like shell (real `busybox sh` if present, else a Go interpreter of the ~25 commands used) reproducing the quirks `camsh` handles (echo, wrapping, `ESC[6n`, IAC), `nc -l` listeners, `/proc/mtd` (8 partitions), `dd`/`md5sum` on a file-backed fake `/dev/mtd7`, with fault injection (drop, short transfer, bad hash, slow, silence leading to manual mode);
- control server on 8091 and an RTSP server with Digest + backchannel; `tests/native/rtsp_auth_server.c` is the conformance oracle for the Go client, and a test compares RTP/RTCP framing against the C `akam-talk`.

### 8.3 TUI
Golden tests: bubbletea models driven by scripted key messages and rendered to an 80x24 text screen, compared with `testdata/*.golden` (colours stripped, plus a separate colour-map test); `--plain` transcripts compared with the bash wizard's on the same scenarios.

### 8.4 SD
FAT32 writer into a sparse file, validated with `fsck.vfat -n`, mtools when available, and the in-binary reader. A privileged CI job runs prepare/verify/wipe on `/dev/loopN` (loop allowed only under a `testloop` build tag, never in release builds).

### 8.5 CI (Linux only)
`go vet`, `staticcheck`, `go test -race`, cross-compile all targets, size budget, reproducibility rebuild, `govulncheck` (advisory), plus the existing `tools/ci/local_check.sh`. Interop: our SSH client against OpenSSH and Dropbear; our sshsig verifier against `ssh-keygen -Y`. CI never touches a camera.

### 8.6 Real-hardware matrix (manual, recorded in `docs/akam_v1_test_plan.md`)
| Test | Stock + toolkit (telnet 24) | Akam (SSH) |
|---|---|---|
| find-camera (probe, WSD, name) | yes | yes |
| first SSH contact, pin, key-change refusal | n/a | yes |
| led/audio/picture/debug/hostname/rtsp-auth/onvif, output compared with bash | n/a | yes |
| backup (8 partitions twice, copy, verify; `SHA256SUMS` equals bash's) | yes | n/a |
| image build native vs mksquashfs; flash over telnet+card | yes (gated by 3.9) | n/a |
| update/restore over SSH incl. 2222 (UART rescue at hand) | n/a | yes |
| talk/say (mic, file, test tone, wrong password, auth off) | n/a | yes |
| SD prepare/verify/wipe on 3 readers (USB SD, built-in mmc, USB stick refused) | yes | yes |
| platforms: Linux x86_64, aarch64 (Raspberry Pi); then macOS arm64, Windows 11 | per phase | per phase |

---

## 9. Phased plan

Each phase ends with the bash wizard still passing `tools/ci/local_check.sh`. AC = acceptance criteria.

### P1 - Core, SSH, daily commands (Linux x86_64 + aarch64)
Commands: `help`, `version`, `status`, `cameras`, `rename-camera`, `register`, `find-camera` (probe, name, WSD), `led`, `audio`, `picture`, `debug`, `hostname`, `rtsp-auth`, `onvif`, `talk`, `say`, `doctor`, `self-test`.
- T1.1 `tools/assistant/` skeleton, Go pin in `deps.lock`, vendoring, CI, size and reproducibility checks, `AKAM_USE_BIN` bridge in the bash script. AC: `akam version` on both arches; two builds give the same sha256; size within budget.
- T1.2 `internal/state`: read/write compatible with bash, multi-camera layout, lock, migration prompts. AC: round-trip with a state made by bash and the reverse; extra keys ignored by bash.
- T1.3 `internal/ui` + bubbletea TUI + plain mode + widgets. AC: golden tests; every prompt defaults to No; plain output equals bash on 10 scripted scenarios.
- T1.4 SSH: client, key generation, pinning, exec/stdin/forward/put/get. AC: works against OpenSSH, Dropbear and the fake camera; changed key refused; key usable by `ssh -i`; no secret in logs.
- T1.5 control-port client and `led`, `audio`, `picture`, `debug`, `hostname`, `rtsp-auth`, `onvif`. AC: same camera-side effects and text as bash on the fake camera and, for the maintainer, on a real camera; `debug on` asks yes/no default No.
- T1.6 RTSP/Digest/backchannel + G.711 + WAV + optional capture + `talk`/`say`. AC: frames byte-compatible with `akam-talk`; `talk test` and `say file.wav` need no external tool; mic works with `arecord`/`ffmpeg` when present; error codes mapped.
- T1.7 `find-camera`. AC: classification matches bash on stock/Akam/unknown fixtures; no traffic before the user chose; subnet scan only behind an explicit yes.
- Bash meanwhile: unchanged and the reference; both share state.

### P2 - Kit, image, backup/flash/update over SSH
Commands: `get-kit`, `image`, `update`, `restore` (SSH route), `verify`, `backup` for a camera already running Akam (SSH `dd`).
- T2.1 download + SSHSIG + safe tar/xz + manifest checks. AC: every refusal case of `verify_kit.sh` is refused (tests ported); cross-verified with `ssh-keygen -Y` both ways; unsigned/test gating identical.
- T2.2 squashfs spike and written decision (3.9). AC: decision record with CI mount results; no default change before a real flash succeeded.
- T2.3 `image`. AC: on `tests/fixtures/app_fake*` and a real dump, file list/modes/hashes equal the `build_firmware.sh --kit` result; size 5242880; report fields equal.
- T2.4 `update`/`restore` over SSH with 2222, detached flash, polling, read-back md5, manual power-cycle message. AC: fake-camera scenarios (success, drop during poll, hash mismatch, refused guard); one real update succeeds with a log matching the bash script's; zero reboot commands (allow-list test).
- T2.5 `verify`. AC: same outcomes as bash on every scenario.
- Bash meanwhile: still the supported path for SD, telnet and first install.

### P3 - Telnet, SD, first install from stock
Commands: `backup` (telnet), `flash` (telnet + card), `prepare-backup-sd`, `prepare-flash-sd`, `wipe-sd`, `telnet`, whole menu.
- T3.1 telnet session with manual mode. AC: replays `tests/camsh_test.sh` transcripts; manual mode after 2 failures or `--manual-telnet`; output never invented; the YES is never auto-sent (test).
- T3.2 nc transfers with size+hash and retries. AC: injected faults detected, 3 tries, no partial file left.
- T3.3 `backup`. AC: folder structure and checksums identical to bash on the same fake and on a real stock camera.
- T3.4 `internal/sdcard`: enumeration, guards, elevation, helper, MBR/FAT32, verify, wipe, toolkit tarball. AC: guard matrix; loop-device CI test; a card made by the binary boots the stock camera into telnet 24 and passes `sd_card.sh verify`; password never in argv/env/log; wipe leaves zeros.
- T3.5 `flash` over telnet + card with all gates, `restore`. AC: fake-camera end to end; one real flash from stock with the binary only; no reboot sent.
- T3.6 first install end to end. AC: the maintainer takes a spare camera from stock to verified Akam with nothing installed but the binary (plus optional ffmpeg).
- Bash meanwhile: shipped; after T3.6 the root `./akam` becomes the launcher (1.3), README leads with the binary, old dispatcher kept as `tools/wizard/akam-legacy`.

### P4 - macOS and Windows
- T4.1 macOS arm64/amd64: terminal specifics, `diskutil` unmount + raw `/dev/rdiskN` helper (or the diskutil fallback of 3.7), Gatekeeper docs. AC: full P1-P3 on a Mac; SD prepared with a USB reader; talk through `ffmpeg avfoundation`.
- T4.2 Windows amd64: VT console, ssh-agent pipe, UAC re-launch for the SD helper or files-only fallback, path/case rules for backups and images, no `sudo` in messages. AC: full P1-P3 on Windows 11; SD either in-binary or the documented fallback, verified.
- T4.3 CI cross-build and smoke tests (QEMU/wine where possible), release artifacts and checksums for all targets. AC: size budget met; signatures verify.

### 9.6 Retirement of the bash wizard
Not part of P1-P4. Decided after P3 plus one release cycle. `tools/sd`, `tools/flash_scripts` stay anyway: parts are shipped to the card and the camera (`flash_guard.sh`, `preflight_check.sh`, `restore.sh`, `install_with_flash_tool.sh`).

---

## 10. Open questions (each with a recommendation)

1. **Module path and release tags.** Recommendation: module in `tools/assistant/`, binary tags `assistant-vX.Y.Z` separate from kit tags `vX.Y.Z`, so a binary fix does not re-cut the signed kit.
2. **Image builder if the squashfs spike fails (3.9).** Ship static `mksquashfs`/`unsquashfs` in the kit, or keep requiring `squashfs-tools`? Recommendation: ship them in the kit (GPL-2.0 separate executables, source offer): Linux users have them, Mac/Windows users do not. Decide after T2.2.
3. **Camera host key type.** Which key types does `akam_ssh.sh` generate? Recommendation: read `tools/firmware_patch_templates/akam_ssh.sh` before T1.4 and accept only that type (no RSA); no camera access needed.
4. **macOS/Windows signing.** Recommendation: unsigned for the first release with documented Gatekeeper/SmartScreen steps; checksums + SSHSIG always; buy certificates only if users complain.
5. **Toolkit payload.** Keep downloading `tasarren/lsc-tuya-toolkit` at the pinned commit, or embed? Recommendation: keep downloading (pin + tree hash); embed only after checking its licence.
6. **Windows SD.** Recommendation: files-only in the first Windows release (user formats FAT32 with the OS), in-binary raw formatting later.
7. **Binary vs script name in a source checkout.** Recommendation: as in 1.3 (release asset `akam`, build output `tools/assistant/bin/akam`, `AKAM_USE_BIN=1` bridge, launcher swap after P3).

### 10.1 Decisions (2026-11-11)

The user delegated these choices to the coordinator. Every recommendation of section 10 is adopted as written:
the binary and the kit get separate release tags; if the squashfs spike fails, static squashfs tools ship in
the kit; host key types follow what `akam_ssh.sh` generates; the first macOS/Windows binaries are unsigned
(documented); the toolkit stays a pinned download, not embedded; Windows starts with files-only SD
preparation; naming as in section 1.3. Work starts with P1.
