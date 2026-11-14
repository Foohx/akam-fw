# Hardware test checklists

Tickable checklists for people who test Akam on real hardware. Tick a box only after you have seen the result on
your own camera. What was already tested, and where: [testing status](testing-status.md).

Send results as an issue or a pull request. Remove Wi-Fi names, passwords, MAC addresses and real IP addresses from
anything you post. **Never attach a backup, a dump or an image.**

## Camera checklist (acceptance)

| # | Criterion | How to check | Done |
|---|---|---|---|
| 1 | Hardware reference confirmed (`3215672.3`, SI `D26228`) | Label; `/proc/mtd`; the wizard's hardware verdict ([backup guide](manual/backup-uart.md)) | [x]|
| 2 | 8 partitions backed up with SHA-256 | `./akam backup`, or the [backup guide](manual/backup-uart.md) checklist | [x]|
| 3 | Boots without a microSD | Remove the card after provisioning, power cycle, `ssh` in, `akam_hwcheck.sh` | [x]|
| 4 | Wi-Fi connects automatically | Camera appears on the network after power-on; `wpa_supplicant` and `udhcpc` PASS in `akam_hwcheck.sh` | [x]|
| 5 | H.264 1080p stream | `ffprobe rtsp://<camera-ip>:554/` and a viewer, 1920x1080 | [x]|
| 6 | Microphone audible (the L16 track exists; listening still to do) | Listen to the stream audio | [ ] |
| 7 | Night mode works | Cover the lens / darken the room: IR-cut switches, image stays usable | [x]|
| 8 | Port 8091 works (read-only from the network) | `akam_hwcheck.sh` LIST check and `debug:` line, `nc <camera-ip> 8091` `GET` from the PC works and `SET` is refused, `./akam led` still works, `./akam debug on` then a PC `SET` works and stops after `off` | [x]|
| 9 | Dropbear starts by itself | `dropbear` PASS and port 22 PASS in `akam_hwcheck.sh`; `/tmp/akam_ssh.log` | [x]|
| 10 | SSH root by key | `ssh -i ~/.ssh/akam root@<camera-ip>`; a password attempt must fail | [x]|
| 11 | Persists after a power cut | Cut power, reconnect: same key accepted, same host fingerprint | [x]|
| 12 | Telnet unreachable and never re-enabled (gone at boot ; the Wi-Fi reconnect case is still to check) | Ports 23 and 24 closed in `akam_hwcheck.sh` after boot, `--boot-window`, and again after a Wi-Fi reconnect; `nmap` from the PC | [ ] |
| 13 | FTP off | Port 21 closed (same checks) | [x]|
| 14 | No Internet dependency | Put the camera on a segment with no route out; everything above still works | [ ] |
| 15 | Memory acceptable | Free memory and RSS in `akam_hwcheck.sh` (section 4) | [x]|
| 16 | Image fits in APP | `build_report.txt` free bytes, and a successful flash | [x]|
| 17 | Restore documented and (your decision) rehearsed | [Flash and restore](manual/flash-and-restore.md) | [ ] documented, not rehearsed |


### MQTT (optional, needs a camera and a broker)

Not run on a camera yet. Use a test broker (see [MQTT and Home Assistant](mqtt.md#5-try-it-without-a-broker-a-temporary-mosquitto-on-your-computer)) or Home
Assistant with its Mosquitto add-on.

| # | Check | How | Done |
|---|---|---|---|
| M1 | Off by default: no thread, no socket | Fresh image, no MQTT file: `akam_hwcheck.sh` says `MQTT is off: ak_rtsp holds no outbound TCP connection`; `for p in $(pidof ak_rtsp); do grep -qa akam_klog /proc/$p/cmdline || ls /proc/$p/task; done` (pidof also lists the kernel log filter) has the same number of threads as an image without MQTT (record both) | [ ] |
| M2 | `./akam mqtt set` then `mqtt test` | Wrong password: test says refused; right password: test says ok; `mqtt status` shows `state: connected` | [ ] |
| M3 | Home Assistant entities | One device `akam-XXXX` with all entities of [the table](mqtt.md#3-what-home-assistant-gets); the MAC merges it with the ONVIF device if both are on | [ ] |
| M4 | Controls work both ways | Each select, number, switch and button in Home Assistant changes the real LED, IR LEDs, night mode, volume (hear it), picture (see it), talk switch, beep; a change made with `./akam led status off` shows up in Home Assistant within about 2 s | [ ] |
| M5 | Sensors plausible | Compare CPU load with `top`, Wi-Fi signal with `iwconfig` or `./akam status`, frame rate and bitrate with `./akam video show`; temperature present or absent as the camera reports | [ ] |
| M6 | Availability | Kill `ak_rtsp` on the camera (the wrapper restarts it) or unplug it: the device shows unavailable within about 90 s (last will), then available again | [ ] |
| M7 | Broker restart | Stop and start the broker: the camera reconnects (delay 5 s growing to 60 s) and publishes discovery again | [ ] |
| M8 | One outbound connection, video untouched | `akam_hwcheck.sh` says `MQTT is on: ak_rtsp holds one outbound TCP connection`; frame rate (`video.fps_actual`) and bitrate unchanged compared with MQTT off, over a 10 minute stream; free memory and `ak_rtsp` VmRSS recorded with MQTT off and on (budget: the delta must stay under 100 KB) | [ ] |
| M9 | Hostile input | `mosquitto_pub` to `BASE/volume/set` with `101`, `-1`, `5;reboot`, an empty and a 10 KB payload, to `BASE/reboot/set`, `BASE/uptime/set` and a retained `BASE/beep/set PRESS`: nothing changes, the camera stays connected | [ ] |
| M10 | Broker down at boot | Enable MQTT, power cycle with the broker stopped: the stream and ONVIF come up as usual, MQTT state is `error:connect`, and it connects once the broker is back | [ ] |


## Post-flash check script

`tools/hw_tests/akam_hwcheck.sh` is a read-only POSIX `sh` script. It never writes to flash or CONFIG,
never kills or starts anything, and never prints key material. It prints `PASS`, `FAIL`, `WARN`,
`UNKNOWN` and `INFO` lines, then facts, and exits 0 if there is no `FAIL`. It is **not** installed in
the image, and it ran on the camera . Run it without copying anything:

```bash
ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh | tee hwcheck-1.txt
```

or copy it to `/tmp` first with `ssh ... 'cat > /tmp/hwcheck.sh' < tools/hw_tests/akam_hwcheck.sh`, or
from the UART console off the card: `sh /mnt/akam_hwcheck.sh`.

It checks, in sections: listening sockets (22, 554 and 8091 expected; 21, 23, 24 must be closed; 80
reported as WARN if open), processes (`ak_rtsp`, `dropbear`, `wpa_supplicant`, `udhcpc` present;
`anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd` absent), resident memory, mounts and `/proc/mtd`, Dropbear
files and modes, whether root's shell is in `/etc/shells`, the maintenance flag, a `LIST` to port 8091,
the debug flag of 8091 (`debug:` info line, WARN while it is on), entropy, the Akam logs, and system facts.

Details worth knowing: the Dropbear key path is judged from **root's HOME in `/etc/passwd` as seen at
runtime** (`akam_ssh.sh` moves it to the private `/tmp/akam_home`), so `/` and `/etc` being 0775 is
only `INFO`; a group/other-writable HOME or key file is a `FAIL`. The `ak_rtsp` CPU use is the delta of
`/proc/PID/stat` over ~2 s (no `top`). The night check reads `night.mode` and samples `night.state` four
times over ~20 s through 8091 (each sample is a short connection; 8091 serves 4 clients at once, so
no other client is disturbed) and WARNs if it changed more than once; `HWCHECK_NIGHT_SAMPLES=0` disables it.

Run order (the first run is expected to WARN about the maintenance flag if you left it in place):

1. Right after the first boot: `akam_hwcheck.sh`. Save the output.
2. **Exposure window**: reboot, and start the sampler as early as you have a shell (UART console), with
   the output on the card, not in RAM:
   ```sh
   sh /mnt/akam_hwcheck.sh --boot-window 180 > /mnt/bootwindow.log 2>&1 &
   ```
   It samples ports 21, 23 and 24 every second (default 120 s if no number is given) and prints the
   uptime of each sample plus first/last seen per port. A forbidden port open on the first line means
   the window began before you started measuring: the result is then a lower bound.
3. After removing `AKAM_MAINTENANCE`: power cycle, run `akam_hwcheck.sh` and `--boot-window` again.
   Ports 21, 23 and 24 must be closed.
4. Power-cut test, then repeat step 3 without the card inserted.
5. Reconnect test: restart your router or Wi-Fi and run the port check again to see that nothing reopens.
6. Manual checks the script only reminds you of: microphone, night mode, 8091 `LIST` / `GET` from the PC, `SET` refused from the PC, `SET` after `./akam debug on`,
   a stream viewer for 1080p.

Compare the port check from outside too: `nmap -p 21,22,23,24,80,554,8091 <camera-ip>`.


## Assistant checklists

Checklists for the `akam` program on Windows, macOS and Linux. A platform is called **ready** only when its whole table
passes. Tested platforms today: [assistant](assistant.md#tested-platforms).

### What you need

- A spare camera on [supported hardware](compatible-cameras.md) (stock, with the maintenance toolkit card route) and a camera that already
  runs Akam. A UART adapter at hand for the flash tests ([UART notes](hacking.md#uart-access)).
- One microSD reader of each kind (a built-in slot where the machine has one, a USB reader) and one plain USB stick.
- A backup of any camera you use, kept on two media ([backup](manual/backup-uart.md)). A flash that is interrupted can brick a camera.
- Interim rule: on Windows and macOS the first install uses an image built on Linux or in WSL (`--image FILE`) until the native
  builder passes its gate.

#### How to run and report

1. Download the binary of your target from the release and verify its SHA256 against `SHA256SUMS`.
2. Go through the table of your OS in order. For each row, write **PASS**, **FAIL** or **SKIP (why)** in "Done" and note what you saw.
   A row that needs hardware you do not have is SKIP, not PASS.
3. For a FAIL give: the row number, the exact text on screen (copy it), `akam version`, `akam doctor`, the OS version and build,
   whether the console was Windows Terminal, the classic console, PowerShell, Terminal or iTerm2, and the end of the log in the
   state folder (it is redacted; read it before sending).
4. Remove Wi-Fi names, passwords, MAC addresses and real IP addresses from everything you post. Never attach a backup, an image or a key.
5. Send the filled table (an issue or a pull request that adds your results below the table, with your OS and date).
   Results also go in [the test plan](hardware-test-checklists.md).

### H1 - Windows 11 amd64 (repeat on windows/arm64 where hardware exists; Windows 10 1809+ spot check)

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

### H2 - macOS arm64 (and amd64 on an Intel Mac, or the amd64 binary under Rosetta for everything except real Intel SD behaviour)

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

### H3 - Linux regression (amd64, and arm64 such as a Raspberry Pi)

Repeat the daily-use checks of the Linux wizard and add:

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

### H3b - Doctor hints, Wi-Fi signal and clock (any OS, one real camera)

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H3b.1 | `doctor` with `ffmpeg`, `arecord`, `mksquashfs` missing (use a PATH without them) | each hint names the right package for the OS (alsa-utils, squashfs-tools / squashfs, Gyan.FFmpeg); on Windows the squashfs tools point to `--builder native` or WSL, never a fake `winget install` | [ ] | |
| H3b.2 | Camera options with the camera close to the access point | `Wi-Fi signal: ... (good)` and no warning; compare the dBm with the router's view | [ ] | |
| H3b.3 | Move the camera (or add a wall) until the signal is below -70 dBm, open Camera options | level `weak` or `poor`, the warning names moving the camera or a repeater and, with the default video settings, `./akam video bitrate 1024 gop 1`; nothing changes on the camera | [ ] | |
| H3b.4 | Same, in the dark (night mode, fps around 10) | the warning says the low frame rate at night is the exposure, not the Wi-Fi | [ ] | |
| H3b.5 | Right after a power-up without time sync, open Camera options | `Clock: not set`; asked `Set the camera clock from this computer now?` with default No; Enter changes nothing; `y` sets it (also confirms busybox `date -u -s 'YYYY-MM-DD hh:mm:ss'` works on the camera), the next screen shows `Clock: set, +/-1 s` | [ ] | |
| H3b.6 | Reboot the camera (by hand) with time sync off, then on | the clock is unset again when off; with `ntp set ntp.example.net` (a server you chose; use a real one for the test) it is right after a minute | [ ] | |
| H3b.7 | End of `verify` of an install or update | time sync is offered, default No; there is no default server: `y` asks for the server(s) to use, and an empty answer changes nothing; `y` then a LAN server address (or a server you name) uses it | [ ] | |

### H4 - End to end per OS

Run once on each OS that passed its table. Windows and macOS use an externally built image until the native builder gate.

| # | Check | Expected result | Done | Notes |
|---|---|---|---|---|
| H4.1 | Stock camera to verified Akam with only the binary: card, backup, image (import), flash card, flash, manual power cycle, `verify` | the camera runs Akam; ports 22, 554, 8091 only by default | [ ] | |
| H4.2 | Restore to stock from the stock backup made in H4.1 | the camera boots the original firmware; read-back md5 equal | [ ] | |
| H4.3 | At no point did the program reboot or power-cycle the camera, and every yes/no default was No | yes | [ ] | |


## Assistant screen checklist

A short check of the `akam` home screen and its main screens on the terminals people really use. The automated tests cover
the logic and the layout; only a person can say whether colours, glyphs and keys feel right in a real terminal. Nothing
here touches a camera: use `--dry-run` where a step would write (the red box then does not open) or press `n`/Esc at every
question. For hardware tests see the [assistant checklists](#assistant-checklists).

### Terminals to run

| # | Terminal | Notes |
|---|----------|-------|
| 1 | Windows 10, conhost (the classic console window, `akam.exe` from cmd or PowerShell) | uses the console's VT mode; try a raster font and Consolas |
| 2 | Windows 10, Windows Terminal | PowerShell and cmd tabs |
| 3 | macOS Terminal.app | also iTerm2 if you have it |
| 4 | A Linux terminal (gnome-terminal, konsole, xterm...) | |
| 5 | tmux (inside any of the above, `TERM=screen-256color` or `tmux-256color`) | resize the pane while the program runs |
| 6 | 16 colours: `TERM=linux` on the Linux console, or `TERM=xterm` with a 16-colour palette | glyphs fall back to ASCII on the Linux console |
| 7 | No colour: `NO_COLOR=1 akam` (PowerShell: `$env:NO_COLOR=1`) | the screen must stay readable and keep its structure |
| 8 | Small window: 60 columns by 16 lines, then 40 by 8 | |

Also run once with `AKAM_ASCII=1` (forces ASCII glyphs) and once with `--plain` (no screen, line-by-line text).

### What to look at, per screen

Write PASS, FAIL or SKIP (why) and what you saw. "Fits" means nothing wraps badly, nothing is cut mid-word at the right edge
and no stray characters (`?`, boxes, `[`...) appear.

| Screen | How to get there | Keys to press | Expected |
|--------|------------------|---------------|----------|
| Home | run `akam` | Up/Down, `j`/`k`, a digit 1 to 8 (shortcut, then answer `n` or Esc to the first question), `?`, `q` | Header (product, camera name, address, badges) over a list; one line of keys at the bottom; the selected row is clearly marked; the status colours (ok, warning, error) differ and each has a word or glyph, not colour alone; `?` opens details and Esc closes them; `q` quits; nothing says "Press Enter" |
| Camera chooser and scan | `c` on the home | `s` then `n` (no packet is sent), `s` then `y` (scan of your own network; stop with Esc), `o` then `n`, `t` (type an address, Esc), Esc | Empty state tells you to press `s` or `t`; the question defaults to No (Enter, Esc, `q` all mean No); the scan shows a status line with a percentage and rows appear as cameras answer; Esc stops it at once and goes back |
| Options form | choose "Camera options" (needs a camera, or use `--dry-run` with a remembered camera) | Up/Down, Left/Right or Space on a choice, type a number and Enter, `p` (password row is hidden), Esc | Six groups (Video, Picture, LED, Audio, Network, Security) on one screen that scrolls; a change shows `SET ...` then a one-line result; typed passwords never show, not even as `*`; Esc leaves without asking anything |
| Backup progress | "Back up / restore", then back up (a camera, or `--dry-run`) | Esc once while reading, `?`, resize the window | Steps listed with a spinner on the current one; a bar with size, elapsed time and time left; Esc says it stops after the current step and nothing on the camera changes; `?` shows the log and Esc closes it; the bar redraws cleanly after a resize |
| Red box | "Prepare the backup microSD card" or "Update this camera" until the box (use a spare card; or `--dry-run`, where it must **not** open and only a `WOULD ask` line shows) | Enter, Esc, `q`, Ctrl-C, then run again and press `y` only on a card you can erase | The box is red (bold uppercase title in no-colour and ASCII), shows the command, and No is highlighted; Enter, Esc, `q` and Ctrl-C all mean No and nothing happens; waiting a minute changes nothing; only `y` (or Left then Enter) means yes |
| Which firmware? | "Set up a new camera" (a card-less check: stop at the list) or "Update this camera" on an Akam camera, with and without a network | Up/Down, a digit, Enter, `/` (filter), Esc; pick an older release, then answer Enter (No) | Title with what the camera runs; `Akam X.Y.Z (latest)` first and selected, older versions marked `older`, then downloaded kits, "A kit file or folder...", "An image file you built (.squashfs)...", and in Update with a verified stock backup "Stock firmware (from your backup)  - never tested on a real camera"; no stock row in the first install; offline or rate-limited: a warning line, only the local rows and a "Try again to read the releases" row; with an image already built from your backup and no builder, that image is the first row; a release whose image cannot be built here is refused and nothing changes; an older release asks a downgrade yes/no with No as the default and nothing is downloaded on No; an image path shows its md5 and a "you take responsibility" question (No by default); Esc goes back and nothing changes |
| SSH key | in "Camera options", Security group, look at the row `SSH key` (`shared (~/.ssh/akam)`, `this camera only (...)` or `custom (...)`), press `k` | `k`, Up/Down and Enter on the question, a passphrase (not shown), `y`/`n` | A read-only row; `k` asks "Which SSH key should open this camera from now on?", then a yes/no (default No) that says what changes, then the write (no stop offered while it runs), then "The new key works ..."; a last yes/no (default No) offers to remove the old key; Esc and No change nothing; no key or passphrase appears in the transcript |
| Configs | "Configs" on the home (digit `9`), then "Edit the global config" (no camera needed) | Up/Down, Left/Right on a choice, type a number and Enter, `s` (passwords, Wi-Fi), `c` (copy from a camera), Esc | The Options-style form on the template: every row starts as "not set" or its saved value; a change is saved at once (no write to any camera); a bad number is refused and the row snaps back; `s` asks "Keep ... in the global config? They are stored on this computer in a private file; anyone with access to your account can read it." with default No, and a typed password is never shown; "Apply the global config to this camera" lists only the differences (passwords as `password: set` or `asked`), asks a yes/no (default No), the video restart is a separate question (default No), and no write is offered a stop while it runs; the Options title then reads `settings: local (from global)` |
| Error screen | stop a camera (or type a wrong address in the chooser) and run "Update this camera" | `l`, Esc, `r`, `c` | Red title in words, what happened, what to do, a muted technical line; `l` opens the log tail, Esc closes only the log; `r` retries; `c` offers another camera; Esc goes back home; no password or key appears anywhere |
| Write in progress (hardware plan only) | during a flash write | Esc, Ctrl-C, `q` | Nothing stops or quits; the footer says the write cannot be interrupted safely; the keys work again when it ends |

### Checks that apply to every screen

- **Colour:** in 256 colours the palette is readable on both a light and a dark background; in 16 colours the red box and
  errors are still red (not invisible); with `NO_COLOR=1` there is no escape-sequence colour at all and warnings still read
  as warnings (`[!]`, `[x]`, `[ok]` or words).
- **Glyphs:** boxes and spinner look right (UTF-8), or fall back to plain ASCII on the Linux console and with `AKAM_ASCII=1`.
- **Width:** a camera name with accents or CJK characters keeps the columns aligned.
- **Resize:** shrink and grow the window while on the home, the chooser and the form. The screen redraws without leftovers.
  At 40 by 8 it keeps its title and its key line.
- **Keys:** arrow keys, Enter, Esc, `q`, `?`, Ctrl-C and the mouse wheel do what the key line says. On Windows conhost also try
  Esc while typing in a field. In tmux check that Esc is not delayed for a long time (set `escape-time 10` if it is).
- **Quit:** `q` or Ctrl-C leaves the terminal as it found it: cursor visible, no colour left on, no alternate screen stuck, typed
  characters echoed again.
- **No camera needed for safety:** on the red box never press `y` unless you meant to; the tester must be able to read the
  command in the box before answering.

