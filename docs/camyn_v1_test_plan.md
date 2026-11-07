# Camyn V1: Test Status, Hardware Test Plan and Resource Report

**Nothing in Camyn V1 has been validated on hardware.** This page separates what was exercised on the
build host from what must be checked on the camera. Tick a box only after you have seen the result on
your own camera.

---

## 1. Status: host tests versus hardware tests

### Tested on the build host (fixture / fake binaries / compile)

| What | How | Result |
|---|---|---|
| `ak_rtsp`, `flash_tool` cross-build (ARM EABI5, soft-float, static) | `make -C src/ak_rtsp ak_rtsp flash_tool` | compiled; not run |
| Dropbear 2026.94 static build | `tools/dropbear/build_dropbear.sh` | compiled; not run |
| `camyn_ssh.sh` logic (first boot, second boot, duplicates, missing binaries, read-only CONFIG, SD keys valid/invalid/`command=`, modes) | `bash tests/ssh_init_test.sh` against a fake root with fake `dropbear` | passed on host |
| `patch_service_sh.sh` and `camyn_lockdown.sh` (variants of `service.sh`, idempotence, flag present/absent, exclusion list) | `bash tests/lockdown_test.sh` with fake `ps`/`kill` and a synthetic `service.sh` | passed on host |
| `build_firmware.sh` (permissions, key refusal, round-trip, size guard, padding to 5 242 880) | `bash tests/build_firmware_test.sh` on `tests/fixtures/app_fake` with stub binaries | passed on host |
| `camyn_hwcheck.sh` | `bash tests/hwcheck_test.sh` with fake `ss`/`ps`/proc | passed on host |

The fixture is synthetic: it contains no vendor files, so these tests say nothing about the real
`service.sh`, the real free space, or kernel behaviour.

### To test on the camera (V1 acceptance criteria)

| # | Criterion | How to check | Done |
|---|---|---|---|
| 1 | Hardware reference confirmed (`3215672.3`, SI `D26228`) | Label; `/proc/mtd`; UART boot log ([backup guide](camyn_v1_backup.md)) | [ ] |
| 2 | 8 partitions backed up with SHA-256 | [Backup guide](camyn_v1_backup.md) checklist | [ ] |
| 3 | Boots without a microSD | Remove the card after provisioning, power cycle, `ssh` in, `camyn_hwcheck.sh` | [ ] |
| 4 | Wi-Fi connects automatically | Camera appears on the network after power-on; `wpa_supplicant` and `udhcpc` PASS in `camyn_hwcheck.sh` | [ ] |
| 5 | H.264 1080p stream | `ffprobe rtsp://<camera-ip>:554/` and a viewer, 1920x1080 | [ ] |
| 6 | Microphone audible | Listen to the stream audio | [ ] |
| 7 | Night mode works | Cover the lens / darken the room: IR-cut switches, image stays usable | [ ] |
| 8 | Port 8091 works | `camyn_hwcheck.sh` LIST check, then manual `GET` / `SET` | [ ] |
| 9 | Dropbear starts by itself | `dropbear` PASS and port 22 PASS in `camyn_hwcheck.sh`; `/tmp/camyn_ssh.log` | [ ] |
| 10 | SSH root by key | `ssh -i ~/.ssh/camyn root@<camera-ip>`; a password attempt must fail | [ ] |
| 11 | Persists after a power cut | Cut power, reconnect: same key accepted, same host fingerprint | [ ] |
| 12 | Telnet unreachable and never re-enabled | Ports 23 and 24 closed in `camyn_hwcheck.sh` after boot, `--boot-window`, and again after a Wi-Fi reconnect; `nmap` from the PC | [ ] |
| 13 | FTP off | Port 21 closed (same checks) | [ ] |
| 14 | No Internet dependency | Put the camera on a segment with no route out; everything above still works | [ ] |
| 15 | Memory acceptable | Free memory and RSS in `camyn_hwcheck.sh` (section 4) | [ ] |
| 16 | Image fits in APP | `build_report.txt` free bytes, and a successful flash | [ ] |
| 17 | Restore documented and (your decision) rehearsed | [Flash and restore](camyn_v1_flash_and_restore.md) | [ ] |

---

## 2. Post-flash procedure

`tools/hw_tests/camyn_hwcheck.sh` is a read-only POSIX `sh` script. It never writes to flash or CONFIG,
never kills or starts anything, and never prints key material. It prints `PASS`, `FAIL`, `WARN`,
`UNKNOWN` and `INFO` lines, then facts, and exits 0 if there is no `FAIL`. It is **not** installed in
the image, and it has never run on a camera. Run it without copying anything:

```bash
ssh -i ~/.ssh/camyn root@<camera-ip> 'sh -s' < tools/hw_tests/camyn_hwcheck.sh | tee hwcheck-1.txt
```

or copy it to `/tmp` first with `ssh ... 'cat > /tmp/hwcheck.sh' < tools/hw_tests/camyn_hwcheck.sh`, or
from the UART console off the card: `sh /mnt/camyn_hwcheck.sh`.

It checks, in sections: listening sockets (22, 554 and 8091 expected; 21, 23, 24 must be closed; 80
reported as WARN if open), processes (`ak_rtsp`, `dropbear`, `wpa_supplicant`, `udhcpc` present;
`anyka_ipc`, `telnetd`, `tcpsvd`, `ftpd` absent), resident memory, mounts and `/proc/mtd`, Dropbear
files and modes, whether root's shell is in `/etc/shells`, the maintenance flag, a `LIST` to port 8091,
entropy, the Camyn logs, and system facts.

Run order (the first run is expected to WARN about the maintenance flag if you left it in place):

1. Right after the first boot: `camyn_hwcheck.sh`. Save the output.
2. **Exposure window**: reboot, and start the sampler as early as you have a shell (UART console), with
   the output on the card, not in RAM:
   ```sh
   sh /mnt/camyn_hwcheck.sh --boot-window 180 > /mnt/bootwindow.log 2>&1 &
   ```
   It samples ports 21, 23 and 24 every second (default 120 s if no number is given) and prints the
   uptime of each sample plus first/last seen per port. A forbidden port open on the first line means
   the window began before you started measuring: the result is then a lower bound.
3. After removing `CAMYN_MAINTENANCE`: power cycle, run `camyn_hwcheck.sh` and `--boot-window` again.
   Ports 21, 23 and 24 must be closed.
4. Power-cut test, then repeat step 3 without the card inserted.
5. Reconnect test: restart your router or Wi-Fi and run the port check again to see that nothing reopens.
6. Manual checks the script only reminds you of: microphone, night mode, 8091 `LIST` / `GET` / `SET`,
   a stream viewer for 1080p.

Compare the port check from outside too: `nmap -p 21,22,23,24,80,554,8091 <camera-ip>`.

---

## 3. Hypotheses to verify

Everything in Camyn's design that came from reading other projects instead of this camera:

| # | Hypothesis | Matters because | How to check |
|---|---|---|---|
| H1 | The unit is `3215672.3` / SI `D26228` with the same layout as upstream's `.2` | Whole project | Label, `/proc/mtd`, dump |
| H2 | `telnetd` / `ftpd` are launched from `sbin/service.sh` in the APP (not from ROOTFS) | Layer 1 patch; exposure window | Read the dump; `grep` ([build guide](camyn_v1_build.md#5-extract-your-app-partition)); `--boot-window` |
| H3 | The stock watchdog only restarts `_ht_process.ini` entries (not telnetd) | No re-enable | Read dump; boot window |
| H4 | The kernel mounts a gzip SquashFS APP (stock is xz; upstream boots gzip) | Boot | A boot of Camyn's image |
| H5 | Free space in the real APP is sufficient | Image fits | `build_report.txt` from the real dump |
| H6 | The microSD is mounted on `/mnt` when the scripts run (keys at `/mnt/camyn/`, flag at `/mnt/CAMYN_MAINTENANCE`) | SSH provisioning; maintenance flag | UART: `mount`; `/tmp/camyn_ssh.log` |
| H7 | `/etc/config` is writable, and `/etc/config`, `/etc`, `/` have modes Dropbear accepts | Key login | Log lines `mode of ...`; successful login |
| H8 | `/dev/pts` is mounted (interactive shell); `/etc/shells` is absent or lists `/bin/sh` | Login | `ls /dev/pts`; hwcheck; log |
| H9 | Root's `$HOME` is `/` (irrelevant: Dropbear is started with `-D`) | None | n/a |
| H10 | Entropy for key generation is available soon enough on a 4.4 kernel without RTC | First SSH latency | `/tmp/camyn_ssh.log` timing; hwcheck entropy |
| H11 | A factory reset or CONFIG wipe removes `/etc/config/dropbear` | Re-provisioning | Deliberate only if you accept losing CONFIG; otherwise leave unknown |
| H12 | Zig/musl static Dropbear runs on this ARM926 kernel (`getrandom`, `forkpty`) | SSH works at all | First SSH login |
| H13 | `ak_rtsp` behaves as upstream (only the build changed) | Video/audio | Criteria 4 to 8 |
| H14 | RAM cost of Dropbear and the helper shells is small | 64 MiB RAM | Section 4 |
| H15 | A leftover flasher SD would run its hook only with the maintenance flag | Telnet 24 | Test with and without the flag |
| H16 | The camera's BusyBox `tar` supports gzip (`tar xzf -`) | Directory transfer | Try it |
| H17 | Flashing from a running Camyn system (Dropbear in the erased APP) is unsafe | Reflash procedure | Recommended to use UART instead |

---

## 4. Resource report

Measured on the build host (binary sizes), or **not yet known** (everything that needs the camera):

| Item | Value | Source |
|---|---|---|
| `ak_rtsp`, stripped | 129 208 bytes | Build host (the build output is ~1.6 MB with `-g`; the image gets a stripped copy) |
| `dropbearmulti` (dropbear + dropbearkey), stripped | 213 372 bytes | Build host (`dist/dropbear/BUILDINFO.txt`); no `scp`/SFTP |
| Camyn scripts on the camera (`ak_rtsp_wrapper.sh`, `camyn_ssh.sh`, `camyn_lockdown.sh`) | about 17 KB | Build host |
| Free space in the real APP image | **unknown until you build from your dump** | `build_report.txt` |
| Free RAM before / after Camyn services | **to measure** | `camyn_hwcheck.sh` ("MemFree", "Cached") |
| RSS of `dropbear`, `ak_rtsp` | **to measure** | `camyn_hwcheck.sh` ("resident memory") |
| CPU load with an RTSP client | **to measure** | `uptime` / `top` section |

Fill in on your own camera:

| Measure | Stock | Camyn, idle | Camyn, one RTSP client |
|---|---|---|---|
| MemFree (kB) | | | |
| `ak_rtsp` VmRSS (kB) | n/a | | |
| `dropbear` VmRSS (kB) | n/a | | |
| APP free (bytes, from `build_report.txt`) | | | |

The fixture's `build_report.txt` figures are not representative and must not be quoted as real values.
