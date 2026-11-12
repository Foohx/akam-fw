# Assistant hardware test plan (Windows, macOS, Linux)

Plan for the people who test the native `akam` binary (spec 11, section 7.3). The binaries for Windows and macOS are
**built and host-tested, not hardware-tested**: nothing below has been run yet. A platform is called **ready** only when
its whole table passes; until then the release marks it "built, not hardware-tested". Current evidence:
[testing status](testing-status.md). What the program does: [tools/assistant/README.md](../tools/assistant/README.md).

## What you need

- A spare camera on [supported hardware](tested-hardware.md) (stock, with the maintenance toolkit card route) and a camera that already
  runs Akam. A UART adapter at hand for the flash tests ([UART notes](hardware_and_uart.md)).
- One microSD reader of each kind (a built-in slot where the machine has one, a USB reader) and one plain USB stick.
- A backup of any camera you use, kept on two media ([backup](akam_v1_backup.md)). A flash that is interrupted can brick a camera.
- Interim rule: on Windows and macOS the first install uses an image built on Linux or in WSL (`--image FILE`) until the native
  builder passes its gate.

## How to run and report

1. Download the binary of your target from the release and verify its SHA256 against `SHA256SUMS`.
2. Go through the table of your OS in order. For each row, write **PASS**, **FAIL** or **SKIP (why)** in "Done" and note what you saw.
   A row that needs hardware you do not have is SKIP, not PASS.
3. For a FAIL give: the row number, the exact text on screen (copy it), `akam version`, `akam doctor`, the OS version and build,
   whether the console was Windows Terminal, the classic console, PowerShell, Terminal or iTerm2, and the end of the log in the
   state folder (it is redacted; read it before sending).
4. Remove Wi-Fi names, passwords, MAC addresses and real IP addresses from everything you post. Never attach a backup, an image or a key.
5. Send the filled table (an issue or a pull request that adds your results below the table, with your OS and date).
   Results also go in [the test plan](akam_v1_test_plan.md).

## H1 - Windows 11 amd64 (repeat on windows/arm64 where hardware exists; Windows 10 1809+ spot check)

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H1.1 | Download, SmartScreen "More info > Run anyway" per 8.3, `akam version`, `akam doctor` | opens, shows `windows/amd64`, no crash | [ ] | |
| H1.2 | Windows Terminal, classic console host and PowerShell: colours, box drawing, full-screen menu, arrow keys, resize, `q`, Ctrl-C | all render; Ctrl-C leaves a clean prompt | [ ] | |
| H1.3 | Password prompts (stream password, Wi-Fi password): no echo | no echo | [ ] | |
| H1.4 | State folder under `%LocalAppData%\Akam`; two instances at once | second refuses (lock) | [ ] | |
| H1.5 | SSH key creation in `%USERPROFILE%\.ssh`; ACL shows only the user, SYSTEM, Administrators; `ssh -i` (OpenSSH for Windows) works with the key | yes | [ ] | |
| H1.6 | ssh-agent (OpenSSH agent service) with a passphrase key | agent used | [ ] | |
| H1.7 | Camera discovery: TCP scan on Wi-Fi and on Ethernet; machine with WSL/Hyper-V/VPN adapters present; WS-Discovery with firewall prompt, both allow and block | cameras listed with host and firmware; virtual adapters not scanned | [ ] | |
| H1.8 | Backup folder suggestion (local Documents; OneDrive-redirected Documents; USB drive as target) | warns on OneDrive, ACL set, FAT/exFAT warns "not private" | [ ] | |
| H1.9 | `get-kit` on a clean profile (symlink-less extraction), then image import | kit verifies, no symlink error | [ ] | |
| H1.10 | SD guards: try the system disk, an internal NVMe, a USB stick, the card with an Explorer window open on it, a BitLocker To Go card, a write-protected card | system/NVMe refused; stick shows model and size; Explorer-open card fails with a clear drive-letter message before any write; BitLocker refused; write-protect reported | [ ] | |
| H1.11 | UAC: accept; decline (Cancel); run the whole program as Administrator | accept writes; decline writes nothing and says so; admin mode works with no prompt | [ ] | |
| H1.12 | Prepare backup card: result read back through the uncached handle; Windows Explorer shows `AKAMSD`; the stock camera boots it into telnet 24 | yes | [ ] | |
| H1.13 | Wipe, then prepare flash card with Wi-Fi password: `verify` passes; password not on disk elsewhere (`%TEMP%`, state, logs) | yes | [ ] | |
| H1.14 | Full first install of the spare camera from stock with an externally built image (until the native builder passes its gate), backup verified, flash, manual power cycle, `verify` | camera runs Akam | [ ] | |
| H1.15 | Update Akam to Akam over SSH; options edit and apply (LED, video, NTP, ONVIF) | applied, "applies at" texts true | [ ] | |
| H1.16 | Close the console window during an SD write and during the flash wait | card refused by the next verify; camera-side flash completes or aborts safely, message says not to power-cycle | [ ] | |
| H1.17 | Antivirus: Microsoft Defender with real-time protection on: no detection of the unsigned binary, or the documented exclusion/report steps work | recorded | [ ] | |
| H1.19 | Built-in audio, no ffmpeg on the PATH: `doctor` shows `Audio: ... microphone: Windows WASAPI (built in)`; `say` with a WAV, an MP3, a FLAC and an Ogg Vorbis file (stereo, 44.1 and 48 kHz) plays at the right speed and pitch | all four play clearly on the camera speaker | [ ] | |
| H1.20 | `talk 20` with the default microphone, no ffmpeg: speech is heard on the camera, no stutter, pitch normal; try a USB headset and the laptop array as default device; microphone privacy switch off gives a clear error | works with both devices; the error names the Windows privacy setting | [ ] | |
| H1.21 | Ctrl-C during `talk`: returns at once; no `akam` process left; the microphone indicator goes off | yes | [ ] | |
| H1.18 | Start to menu time (cold, Defender on) | under 100 ms of program time, recorded wall time | [ ] | |

## H2 - macOS arm64 (and amd64 on an Intel Mac, or the amd64 binary under Rosetta for everything except real Intel SD behaviour)

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H2.1 | Download in a browser (quarantine) and with `curl`; Gatekeeper steps of 8.3; `akam version` | runs after the documented step; arm64 binary not killed | [ ] | |
| H2.2 | Terminal and iTerm2: colours, full-screen menu, Ctrl-C | ok | [ ] | |
| H2.3 | State folder `~/Library/Application Support/Akam`, key in `~/.ssh`, permissions 0700/0600 | yes | [ ] | |
| H2.4 | SD guards: internal SSD, a USB card reader, the built-in SDXC slot (if any; records whether `Internal` is true), a USB stick, a disk image mounted with Disk Utility | SSD and disk image refused; reader accepted; slot accepted on protocol; stick policy as designed | [ ] | |
| H2.5 | `sudo` elevation in a terminal (password prompt, request on stdin); opt-in `osascript` path | both write the card; decline writes nothing | [ ] | |
| H2.6 | Permissions: no Full Disk Access or Removable Volumes prompt for the raw write (record what appears) | recorded | [ ] | |
| H2.7 | After the write: automount of the new volume does not break verification; `diskutil eject` message correct | verify passes; extra `.fseventsd` harmless | [ ] | |
| H2.8 | iCloud Drive "Desktop & Documents" on: backup suggestion warns and proposes `~/Akam backups` | yes | [ ] | |
| H2.9 | Discovery with Wi-Fi and Ethernet both up; firewall prompt for WS-Discovery | listed; prompt text matches the note | [ ] | |
| H2.10 | First install from stock, update, options, restore, as H1.14/H1.15 | passes | [ ] | |
| H2.11 | `talk`/`say` with ffmpeg installed (`avfoundation`) and without (clear message: macOS needs ffmpeg for the microphone). `say` with a WAV, MP3, FLAC and Ogg file works with no ffmpeg | ok | [ ] | |

## H3 - Linux regression (amd64, and arm64 such as a Raspberry Pi)

Repeat the matrix of spec 09 (section 8.6) and add:

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H3.1 | SD guards on a built-in mmc slot and on a USB reader (`prepare-backup-sd`, answer No at the red box) | the card is listed with model and size; the system disk is never offered; guard messages as before the refactor; No writes nothing | [ ] | |
| H3.2 | Elevation with `sudo` (terminal) and with `pkexec` (graphical session, `AKAM_SD_ELEVATE=pkexec`) | both write the card; declining writes nothing | [ ] | |
| H3.3 | Card prepared by the binary: the stock camera boots it into telnet 24 (backup card) and reads the flash card | yes | [ ] | |
| H3.4 | Home screen, camera chooser (scan and WS-Discovery each asked first, default No), option screens: apply an LED, video and ONVIF change | applied; "applies at" texts true; ports shown match the scan | [ ] | |
| H3.5 | Backup of an Akam camera over SSH (no card) | folder `Akam backups/<camera>/<date>` with 8 files, `SHA256SUMS` ok, marker `kind=akam`, private permissions | [ ] | |
| H3.6 | Update Akam to Akam over SSH with a verified backup recorded | the backup gate re-verifies, red box shows the md5, flash, manual power cycle, `verify` passes | [ ] | |
| H3.8 | Built-in decoders: `say` with a short MP3, FLAC and Ogg Vorbis file whose names have a wrong extension (for example `voice.bin`) | plays, no ffmpeg needed (also with ffmpeg removed from the PATH) | [ ] | |
| H3.9 | `talk 20` on a PipeWire desktop (`pipewire-pulse` running) with no ffmpeg and no arecord on the PATH; `doctor` shows `microphone: PulseAudio / PipeWire (built in)` | voice heard on the camera at normal pitch; the capture stream shows up in `pavucontrol`/`wpctl status` as `akam` and disappears at the end | [ ] | |
| H3.10 | `talk` on a plain PulseAudio desktop, and with the server stopped (`AKAM_TALK_CAPTURE_CMD` unset) | PulseAudio works; without a server it falls back to `arecord`/`ffmpeg` if present, else a clear error | [ ] | |
| H3.11 | `AKAM_TALK_CAPTURE_CMD='arecord -q -t raw -f S16_LE -r 8000 -c 1'` still wins over the built-in backend | the command is used (`Microphone: AKAM_TALK_CAPTURE_CMD`) | [ ] | |
| H3.7 | Native `image` | only when its gate is open; otherwise SKIP | [ ] | |

## H3b - Doctor hints, Wi-Fi signal and clock (any OS, one real camera)

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H3b.1 | `doctor` with `ffmpeg`, `arecord`, `mksquashfs` missing (use a PATH without them) | each hint names the right package for the OS (alsa-utils, squashfs-tools / squashfs, Gyan.FFmpeg); on Windows the squashfs tools point to `--builder native` or WSL, never a fake `winget install` | [ ] | |
| H3b.2 | Camera options with the camera close to the access point | `Wi-Fi signal: ... (good)` and no warning; compare the dBm with the router's view | [ ] | |
| H3b.3 | Move the camera (or add a wall) until the signal is below -70 dBm, open Camera options | level `weak` or `poor`, the warning names moving the camera or a repeater and, with the default video settings, `./akam video bitrate 1024 gop 1`; nothing changes on the camera | [ ] | |
| H3b.4 | Same, in the dark (night mode, fps around 10) | the warning says the low frame rate at night is the exposure, not the Wi-Fi | [ ] | |
| H3b.5 | Right after a power-up without time sync, open Camera options | `Clock: not set`; asked `Set the camera clock from this computer now?` with default No; Enter changes nothing; `y` sets it (also confirms busybox `date -u -s 'YYYY-MM-DD hh:mm:ss'` works on the camera), the next screen shows `Clock: set, +/-1 s` | [ ] | |
| H3b.6 | Reboot the camera (by hand) with time sync off, then on | the clock is unset again when off; with `ntp set pool.ntp.org` it is right after a minute | [ ] | |
| H3b.7 | End of `verify` of an install or update | time sync is offered, default No; `y` then Enter turns on `pool.ntp.org`, `y` then a LAN server address uses it | [ ] | |

## H4 - End to end per OS

Run once on each OS that passed its table. Windows and macOS use an externally built image until the native builder gate.

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H4.1 | Stock camera to verified Akam with only the binary: card, backup, image (import), flash card, flash, manual power cycle, `verify` | the camera runs Akam; ports 22, 554, 8091 only by default | [ ] | |
| H4.2 | Restore to stock from the stock backup made in H4.1 | the camera boots the original firmware; read-back md5 equal | [ ] | |
| H4.3 | At no point did the program reboot or power-cycle the camera, and every yes/no default was No | yes | [ ] | |

## Reporting summary

| Platform | Tester | Date | OS build | Rows passed | Rows failed | Status |
|---|---|---|---|---|---|---|
| Windows 11 amd64 | | | | | | built, not hardware-tested |
| Windows arm64 | | | | | | built, not hardware-tested |
| macOS arm64 | | | | | | built, not hardware-tested |
| macOS amd64 | | | | | | built, not hardware-tested |
| Linux amd64 | | | | | | partly tested |
| Linux arm64 | | | | | | built, not hardware-tested |
