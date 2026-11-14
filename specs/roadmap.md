# Roadmap: known gaps

Only items that are still open in the code or have never been checked on hardware. Items that were fixed are not listed.
Priorities: **P0** blocks calling a release stable or secure; **P1** important; **P2** nice to have. "HW" means a real
camera is needed to close it.

Status: this file is a list of gaps, not a promise or a schedule.

## 1. Release and trust

| Pri | Gap | Detail |
|---|---|---|
| P0 | No release key | `tools/release/allowed_signers` holds no key, so a signed kit cannot be authenticated; installers need `--allow-unsigned` |
| P0 | No published release | `get-kit` and the firmware chooser were only tested against a test server that plays the release host |
| P1 | Release kit never flashed | A second camera has not been flashed from a release kit (only from development kits) |
| P1 | Unsigned PC binaries | Windows SmartScreen and macOS Gatekeeper warn; signing hooks are reserved but empty |
| P2 | Legacy aliases | The `camyn/` SD folder, `CAMYN_MAINTENANCE` and `CAMYN_FLASH_I_KNOW` still work and are marked for removal at the first release, with `tests/upgrade_from_camyn_test.sh` |

## 2. Safety and recovery

| Pri | Gap | Detail |
|---|---|---|
| P0 | No bootloader or U-Boot recovery path | Only APP is rewritten. A damaged bootloader, kernel or flash needs an external SPI programmer or UART work; nothing in Akam rebuilds or restores UBOOT, KERNEL, ROOTFS or DTB |
| P0 | Restore to stock never rehearsed | `restore.sh`, `restore` and `update --stock` have never run on a camera |
| P1 | Over-SSH flash can only be recovered by UART if interrupted half way | By design (`--i-understand-no-uart`); a UART adapter should be at hand for the first flash |
| P1 | Go assistant flash paths never run on hardware | Update, restore, telnet backup and flash, first install, the SD writer (all OSes) |
| P1 | Native image builder never flashed | `akam image --builder native` is opt-in; the hardware gate (a flash on a spare camera with UART) is open, and the QEMU boot check was not done |
| P2 | SD firmware update is unsigned | With the maintenance flag on a card, any `*.bin` flashes the camera. Accepted as physical access; no in-camera signature check is planned |
| P2 | Secrets in backups | The CONFIG dump holds the Wi-Fi password, the stream hash and the clear-text ONVIF password. Backups are private by permissions only |

## 3. Hardware test debt

Never seen on a camera (see `docs/testing-status.md` and `docs/akam_v1_test_plan.md`):

| Pri | Item |
|---|---|
| P0 | Telnet and FTP stay closed after a Wi-Fi reconnect (router restart); exposure window at boot not measured (`akam_hwcheck.sh --boot-window` exists) |
| P0 | Kill and hang tests of `ak_rtsp` (software watchdog exit 70, wrapper respawn backoff), respawn storm |
| P0 | Router off for 2, 10 and 40 minutes, DHCP renewal, wrong Wi-Fi password then corrected (Wi-Fi supervisor under real faults) |
| P1 | 72 h soak: RSS, descriptors, threads, `/tmp` size, CPU, with a viewer reconnecting every minute; log caps measured |
| P1 | Day/night over a whole dusk and dawn cycle (only a covered-lens check was done) |
| P1 | Strict `SET` ranges on `ae.*`, `night.*`; debug-mode expiry and the loopback/remote write rule on the final image |
| P1 | Stdout stall: block the console and check that video continues |
| P1 | Two simultaneous RTSP clients with a slow-loris client; a camera on a segment with no route out |
| P1 | Microphone off (`audio.mic_enable=0`) and the capture supervisor |
| P1 | Video bitrate and GOP settings (frame rate is validated) |
| P1 | MQTT end to end with a real broker and Home Assistant (host-tested only), and its RSS |
| P1 | Per-camera SSH key change over SSH, and configs, on a real camera (host-tested only) |
| P1 | Stack-protector build boot test |
| P2 | Idle wakeups, idle CPU, power draw (the measurement table is empty); Wi-Fi power save experiment |

## 4. Security hardening

| Pri | Gap | Detail |
|---|---|---|
| P1 | No fuzzing of the SOAP and HTTP parsers | Parsers are bounded and unit-tested, but not fuzzed. Do this before enabling ONVIF in a release image by default |
| P1 | MQTT has no TLS | The broker password travels in clear. A TLS client (for example BearSSL, with a user-provided CA or fingerprint pin) was designed but is not built; it must stay inside the size budget |
| P2 | Hardening flags | Stack protector only on network-facing objects; no PIE (`-static-pie` untried), no static analysis in the test script |
| P2 | RTSP Digest uses MD5 without `qop` | Chosen for client compatibility (VLC/live555, go2rtc). Does not protect against an active attacker on the path |
| P2 | Vendor daemons unaudited | Only `anyka_ipc` is removed from the APP; the stock APP's other binaries and libraries are kept. The process allow-list passed on one camera, but nothing proves the unused ones are inert |
| P2 | Dropbear has no per-IP rate limit | Only 3 tries per connection |

## 5. Features not built

| Pri | Item | Note |
|---|---|---|
| P2 | Sub-stream and extra resolutions | The encoder, ISP crop and DMA pool are sized for 1920x1080 only |
| P2 | Mirror and flip | The ISP flip ioctl number is known; its payload and the sensor register sequence are not |
| P2 | More than one RTSP viewer | One client at a time by design (go2rtc restreams) |
| P2 | Snapshot (JPEG), ONVIF Events and motion detection | No JPEG path; motion would be frame differencing on the sub-channel luma or encoder frame-size spikes, off by default |
| P2 | On-screen display (timestamp) and time zone | The vendor OSD module is not used; needs a reverse-engineering session and a clock |
| P2 | Hostname change without power cycle | `udhcpc` cannot be restarted live without dropping the address |
| P2 | 30 fps | Tested, refused: the picture is dark and grainy |
| P2 | Image grain against stock | Cause not proven. Candidates: encoder rate-control sub-struct (`venc_rc=stock` untested), sharpening stock re-applies after a switch |

## 6. Platform support

| Pri | Gap |
|---|---|
| P1 | Windows and macOS (Go assistant): the full-screen interface, SD writer, UAC and `osascript` flows, flash. Linux arm64 untested |
| P1 | Only camera model `3215672.3` is validated. The `3215672.2` boards are untested; any other camera is unsupported |
| P2 | The bash wizard has no macOS support and no configs, per-camera key change or firmware chooser |
| P2 | Dev-only code in the production daemon: `--grab-then-exit`, the SIGUSR1 frame grab to `/mnt`, the alternate AE (`aec_custom.c`) and Makefile dev variants; the kit ships only `ak_rtsp`, `dropbearmulti` and `flash_tool` |
