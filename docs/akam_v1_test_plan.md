# Akam V1: Test Status, Hardware Test Plan and Resource Report

Akam V1 was first validated on a real camera on **2026-11-08** (one unit, list in section 1b). This
page separates what was exercised on the build host, what was seen on that camera, and what is still to
check. Tick a box only after you have seen the result on your own camera.

---

## 1. Status: host tests versus hardware tests

### Tested on the build host (fixture / fake binaries / compile)

| What | How | Result |
|---|---|---|
| `ak_rtsp`, `flash_tool` cross-build (ARM EABI5, soft-float, static) | `make -C src/ak_rtsp ak_rtsp flash_tool` | compiled; not run |
| Dropbear 2026.94 static build | `tools/dropbear/build_dropbear.sh` | compiled; not run |
| `akam_ssh.sh` logic (first boot, second boot, duplicates, missing binaries, read-only CONFIG, SD keys valid/invalid/`command=`, modes) | `bash tests/ssh_init_test.sh` against a fake root with fake `dropbear` | passed on host |
| `patch_service_sh.sh` and `akam_lockdown.sh` (variants of `service.sh`, idempotence, flag present/absent, exclusion list) | `bash tests/lockdown_test.sh` with fake `ps`/`kill` and a synthetic `service.sh` | passed on host |
| Upgrade from an image built under the former name (Camyn): old first-boot card `camyn/` + `CAMYN_MAINTENANCE`, keys persisted in `/etc/config/dropbear`, then a boot without the card | `bash tests/upgrade_from_camyn_test.sh` with the shipped `service.sh` patch, `akam_ssh.sh` and `akam_lockdown.sh` in a fake root | passed on host |
| `build_firmware.sh` (permissions, key refusal, round-trip, size guard, padding to 5 242 880) | `bash tests/build_firmware_test.sh` on `tests/fixtures/app_fake` with stub binaries | passed on host |
| `akam_hwcheck.sh` (key path from root's HOME, `ak_rtsp` CPU from `/proc` deltas, night flap check) | `bash tests/hwcheck_test.sh` with fake `ss`/`ps`/proc and a scripted 8091 | passed on host |
| No-UART reflash over SSH: guard decisions, detach (`setsid`), second Dropbear on 2222, log polling, md5 verification | `bash tests/reflash_ssh_test.sh` with fake `ssh`, `flash_tool`, `dropbear` and a regular file as `/dev/mtd7` | passed on host; **not run on a camera** |
| time64 clock shim ([kernel time64 note](akam_v1_kernel_time64.md)) | `bash tests/time32_shim_test.sh` and `bash tests/dropbear_patch_test.sh`: final binaries scanned for forbidden time64 syscalls, `qemu-arm -strace`, probe verdict logic | passed on host; not run on the camera |

The fixture is synthetic: it contains no vendor files, so these tests say nothing about the real
`service.sh`, the real free space, or kernel behaviour.

### 1b. Validated on the camera, 2026-11-08

Seen on one real unit (Akam image flashed over the telnet routes of the
[flash guide](akam_v1_flash_and_restore.md)):

- boots **without a microSD** card;
- Wi-Fi connects by itself (station mode);
- H.264 1080p video and an **L16 audio track** in the RTSP stream (`ffprobe`);
- the **IR night image** is delivered;
- port 8091: `GET` and `SET` work;
- **Dropbear starts by itself** at boot;
- **SSH as root by ed25519 key** works;
- SSH still works **after a power cycle**, with the host key persisted (same fingerprint);
- without the card only **22, 554 and 8091** are listening;
- **telnet and FTP are off** (21, 23 and 24 closed) in a normal boot;
- `ak_rtsp` uses about **1% CPU** with a client connected (about 90% before the fixes) and the H.264 stream starts fast;
- `anyka_ipc` is absent;
- about **29 MB of RAM free** with everything running;
- the APP image has **2.7 MB free**;
- `flash_tool` reflashed the APP twice live (stock telnet, then maintenance telnet): 0 retries, read-back
  md5 matched, the running system survived the write.

Still to check:

- the **microphone is audible** (the audio track exists; nobody has listened yet);
- **no day/night flapping over a longer period** (hours, across dusk and dawn; use `akam_hwcheck.sh`
  repeatedly, its night check WARNs when `night.state` changes more than once in ~20 s);
- **telnet is never re-enabled after a Wi-Fi reconnect** (restart the router, then check 23 and 24);
- the reflash over SSH from RAM on hardware (host-tested only), and any restore.

### To test on the camera (V1 acceptance criteria)

| # | Criterion | How to check | Done |
|---|---|---|---|
| 1 | Hardware reference confirmed (`3215672.3`, SI `D26228`) | Label; `/proc/mtd`; UART boot log ([backup guide](akam_v1_backup.md)) | [ ] |
| 2 | 8 partitions backed up with SHA-256 | [Backup guide](akam_v1_backup.md) checklist | [ ] |
| 3 | Boots without a microSD | Remove the card after provisioning, power cycle, `ssh` in, `akam_hwcheck.sh` | [x] 2026-11-08 |
| 4 | Wi-Fi connects automatically | Camera appears on the network after power-on; `wpa_supplicant` and `udhcpc` PASS in `akam_hwcheck.sh` | [x] 2026-11-08 |
| 5 | H.264 1080p stream | `ffprobe rtsp://<camera-ip>:554/` and a viewer, 1920x1080 | [x] 2026-11-08 |
| 6 | Microphone audible (the L16 track exists; listening still to do) | Listen to the stream audio | [ ] |
| 7 | Night mode works | Cover the lens / darken the room: IR-cut switches, image stays usable | [x] 2026-11-08 (IR image; flapping over time still to check) |
| 8 | Port 8091 works | `akam_hwcheck.sh` LIST check, then manual `GET` / `SET` | [x] 2026-11-08 |
| 9 | Dropbear starts by itself | `dropbear` PASS and port 22 PASS in `akam_hwcheck.sh`; `/tmp/akam_ssh.log` | [x] 2026-11-08 |
| 10 | SSH root by key | `ssh -i ~/.ssh/akam root@<camera-ip>`; a password attempt must fail | [x] 2026-11-08 |
| 11 | Persists after a power cut | Cut power, reconnect: same key accepted, same host fingerprint | [x] 2026-11-08 |
| 12 | Telnet unreachable and never re-enabled (gone at boot on 2026-11-08; the Wi-Fi reconnect case is still to check) | Ports 23 and 24 closed in `akam_hwcheck.sh` after boot, `--boot-window`, and again after a Wi-Fi reconnect; `nmap` from the PC | [ ] |
| 13 | FTP off | Port 21 closed (same checks) | [x] 2026-11-08 |
| 14 | No Internet dependency | Put the camera on a segment with no route out; everything above still works | [ ] |
| 15 | Memory acceptable | Free memory and RSS in `akam_hwcheck.sh` (section 4) | [x] 2026-11-08 (~29 MB free) |
| 16 | Image fits in APP | `build_report.txt` free bytes, and a successful flash | [x] 2026-11-08 (2.7 MB free) |
| 17 | Restore documented and (your decision) rehearsed | [Flash and restore](akam_v1_flash_and_restore.md) | [ ] |

---

## 2. Post-flash procedure

`tools/hw_tests/akam_hwcheck.sh` is a read-only POSIX `sh` script. It never writes to flash or CONFIG,
never kills or starts anything, and never prints key material. It prints `PASS`, `FAIL`, `WARN`,
`UNKNOWN` and `INFO` lines, then facts, and exits 0 if there is no `FAIL`. It is **not** installed in
the image, and it ran on the camera on 2026-11-08. Run it without copying anything:

```bash
ssh -i ~/.ssh/akam root@<camera-ip> 'sh -s' < tools/hw_tests/akam_hwcheck.sh | tee hwcheck-1.txt
```

or copy it to `/tmp` first with `ssh ... 'cat > /tmp/hwcheck.sh' < tools/hw_tests/akam_hwcheck.sh`, or
from the UART console off the card: `sh /mnt/akam_hwcheck.sh`.

It checks, in sections: listening sockets (22, 554 and 8091 expected; 21, 23, 24 must be closed; 80
reported as WARN if open), processes (`ak_rtsp`, `dropbear`, `wpa_supplicant`, `udhcpc` present;
`anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd` absent), resident memory, mounts and `/proc/mtd`, Dropbear
files and modes, whether root's shell is in `/etc/shells`, the maintenance flag, a `LIST` to port 8091,
entropy, the Akam logs, and system facts.

Details worth knowing: the Dropbear key path is judged from **root's HOME in `/etc/passwd` as seen at
runtime** (`akam_ssh.sh` moves it to the private `/tmp/akam_home`), so `/` and `/etc` being 0775 is
only `INFO`; a group/other-writable HOME or key file is a `FAIL`. The `ak_rtsp` CPU use is the delta of
`/proc/PID/stat` over ~2 s (no `top`). The night check reads `night.mode` and samples `night.state` four
times over ~20 s through 8091 (each sample is a short connection, which briefly replaces any other
8091 client) and WARNs if it changed more than once; `HWCHECK_NIGHT_SAMPLES=0` disables it.

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
6. Manual checks the script only reminds you of: microphone, night mode, 8091 `LIST` / `GET` / `SET`,
   a stream viewer for 1080p.

Compare the port check from outside too: `nmap -p 21,22,23,24,80,554,8091 <camera-ip>`.

---

## 3. Hypotheses to verify

Everything in Akam's design that came from reading other projects instead of this camera:

| # | Hypothesis | Matters because | How to check |
|---|---|---|---|
| H1 | The unit is `3215672.3` / SI `D26228` with the same layout as upstream's `.2` | Whole project | Label, `/proc/mtd`, dump |
| H2 | `telnetd` / `ftpd` are launched from `sbin/service.sh` in the APP (not from ROOTFS) | Layer 1 patch; exposure window | Read the dump; `grep` ([build guide](akam_v1_build.md#5-extract-your-app-partition)); `--boot-window` |
| H3 | The stock watchdog only restarts `_ht_process.ini` entries (not telnetd) | No re-enable | Read dump; boot window |
| H4 | The kernel mounts Akam's xz SquashFS APP (same format as the stock APP; xz support proven by the stock ROOTFS/APP; gzip also expected from the kernel's decompressor tables) | Boot | A boot of Akam's image |
| H5 | Free space in the real APP is sufficient | Image fits | `build_report.txt` from the real dump |
| H6 | The microSD is mounted on `/mnt` when the scripts run (keys at `/mnt/akam/`, flag at `/mnt/AKAM_MAINTENANCE`) | SSH provisioning; maintenance flag | UART: `mount`; `/tmp/akam_ssh.log` |
| H7 | `/etc/config` is writable, and `/etc/config`, `/etc`, `/` have modes Dropbear accepts | Key login | Log lines `mode of ...`; successful login. **Resolved 2026-11-08:** `/` and `/etc` are 0775 on the real unit; the key path is root's HOME `/tmp/akam_home` instead, and key login works |
| H8 | `/dev/pts` is mounted (interactive shell); `/etc/shells` is absent or lists `/bin/sh` | Login | `ls /dev/pts`; hwcheck; log |
| H9 | Root's `$HOME` is `/` | Key path (Dropbear walks up to HOME) | **Superseded:** `akam_ssh.sh` bind-mounts a private passwd whose root HOME is `/tmp/akam_home` |
| H10 | Entropy for key generation is available soon enough on a 4.4 kernel without RTC | First SSH latency | `/tmp/akam_ssh.log` timing; hwcheck entropy |
| H11 | A factory reset or CONFIG wipe removes `/etc/config/dropbear` | Re-provisioning | Deliberate only if you accept losing CONFIG; otherwise leave unknown |
| H12 | Zig/musl static Dropbear runs on this ARM926 kernel (`getrandom`, `forkpty`) | SSH works at all | First SSH login |
| H13 | `ak_rtsp` behaves as upstream (only the build changed) | Video/audio | Criteria 4 to 8 |
| H14 | RAM cost of Dropbear and the helper shells is small | 64 MiB RAM | Section 4 |
| H15 | A leftover flasher SD would run its hook only with the maintenance flag | Telnet 24 | Test with and without the flag |
| H16 | The camera's BusyBox `tar` supports gzip (`tar xzf -`) | Directory transfer | Try it |
| H17 | Flashing from a running Akam system (Dropbear in the erased APP) is unsafe | Reflash procedure | Two live flashes over telnet worked (0 retries, md5 OK, system survived); still no guarantee. UART remains the only safe route; the RAM-only SSH route is host-tested only |
| H18 | With `src/common/time32_shim.c` linked, clock reads and timed waits work on the camera kernel, which acknowledges `clock_gettime64` without executing it ([note](akam_v1_kernel_time64.md)) | Dropbear sessions, night-mode lock | Run `time64_probe` on the camera: `RESULT PASS`; Dropbear login stays open |

---

## 4. Resource report

Measured on the build host (binary sizes), or **not yet known** (everything that needs the camera):

| Item | Value | Source |
|---|---|---|
| `ak_rtsp`, stripped | 129 208 bytes | Build host (the build output is ~1.6 MB with `-g`; the image gets a stripped copy) |
| `dropbearmulti` (dropbear + dropbearkey), stripped | 213 372 bytes | Build host (`dist/dropbear/BUILDINFO.txt`); no `scp`/SFTP |
| Akam scripts on the camera (`ak_rtsp_wrapper.sh`, `akam_ssh.sh`, `akam_lockdown.sh`) | about 17 KB | Build host |
| Free space in the real APP image | **unknown until you build from your dump** | `build_report.txt` |
| Free RAM before / after Akam services | **to measure** | `akam_hwcheck.sh` ("MemFree", "Cached") |
| RSS of `dropbear`, `ak_rtsp` | **to measure** | `akam_hwcheck.sh` ("resident memory") |
| CPU load with an RTSP client | `ak_rtsp` about 1% with one client (was about 90% before the fixes), 2026-11-08 | `akam_hwcheck.sh` (CPU section) |

Fill in on your own camera:

| Measure | Stock | Akam, idle | Akam, one RTSP client |
|---|---|---|---|
| MemFree (kB) | | | |
| `ak_rtsp` VmRSS (kB) | n/a | | |
| `dropbear` VmRSS (kB) | n/a | | |
| APP free (bytes, from `build_report.txt`) | | | |

The fixture's `build_report.txt` figures are not representative and must not be quoted as real values.
