# Akam V1: Flashing and Restoring (manual procedures)

> **Advanced / manual procedure (without the wizard).** `./akam flash`, `./akam update` and `./akam restore`
> run these routes for you, with the same checks and a red yes/no (default No) before any write
> ([README](../README.md#install-in-5-steps)). Use this page to understand them, audit them, or when you cannot run
> the wizard.

> **RISK.** Writing the SPI NOR can brick the camera. Restoring is **not guaranteed** to work (section
> 5). Do not start without a verified 8-partition backup ([backup guide](akam_v1_backup.md)). A UART
> connection ([hardware_and_uart.md](hardware_and_uart.md)) is the only way back from an APP that does
> not boot; the [no-UART variant](#first-flash-without-uart-from-the-stock-firmware) below exists, but
> read what it can and cannot recover (section 5) before choosing it.
>
> Status: `flash_tool` and `install_with_flash_tool.sh` were run on real cameras through the stock
> telnet and the Akam maintenance telnet, with no chunk retries and a matching read-back md5 each time
> (2026-11-07 to 11-09). The reflash over SSH from RAM (`reflash_over_ssh.sh`, no card) was also validated
> on hardware, through `./akam update`. **Restoring has not been rehearsed on a camera.**

---

## 1. Why Akam's own procedure and not the flasher

The [Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) (v0.3.2) is
a good tool, but Akam does **not** use it:

- Its repository and commit constants (`FIRMWARE_REPO`, `FIRMWARE_COMMIT`) are hard-coded to the upstream
  project. It builds upstream's `ak_rtsp`, not this tree, and knows nothing about Dropbear, the SSH
  boot script, or the lockdown patch.
- It patches `ak_rtsp` sources on the fly. Its fourth patch (the `night.c` day/night override) is
  expected to **fail on this source tree**, where the flasher's fixes are already integrated
  (see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).
- It talks to the camera through an unauthenticated Telnet shell on port 24, opened by an SD-card hook,
  which is exactly what Akam closes. A card left over from the flasher keeps that hook.
- The flasher's own post-flash check does not look at SSH.

Akam's procedure is: backup, `build_firmware.sh`, then `install_with_flash_tool.sh` below. The
flasher stays a reference. The image builder still honours the flasher's interface (`APP_EXTRACTED`, `FW_DIR`, output name,
exact size).

## 2. What is written, and what never is

- Only `/dev/mtd7` (APP, 5 242 880 bytes at `0x300000`) is written by the install script.
- **UBOOT, ENV, ENVBK, DTB, KERNEL and ROOTFS are never written by Akam V1.** CONFIG is only written
  by the camera itself at runtime (Dropbear keys).
- Do not use `dd` to write `/dev/mtdblock7` on a running system: upstream documents silent corruption
  from SPI-NOR DMA contention ([details](firmware_building_and_flashing.md#5-the-spi-nor-dma-contention-gotcha)).
  `flash_tool` writes 4 KiB blocks to the raw `/dev/mtd7` and reads each back, retrying on mismatch.

## 3. Prepare the SD card

On the PC, check the image, then copy files to a FAT32 card:

```bash
cd ~/akam-work/out
sha256sum -c ak_rtsp_firmware_*.squashfs.sha256
md5sum ak_rtsp_firmware_*.squashfs          # note it; the camera prints md5sum too

SD=/run/media/$USER/CARD                    # adapt
mkdir -p "$SD/custom_firmware" "$SD/akam" "$SD/original_firmware"
cp ak_rtsp_firmware_*.squashfs "$SD/custom_firmware/"
cp ~/akam-backup/mtdblock*.bin "$SD/original_firmware/"          # for restore
REPO=~/akam-fw                                                   # your checkout of the repository
cp "$REPO/src/ak_rtsp/flash_tool" "$SD/"
cp "$REPO/tools/flash_scripts/install_with_flash_tool.sh" \
   "$REPO/tools/flash_scripts/restore.sh" \
   "$REPO/tools/flash_scripts/flash_guard.sh" "$SD/"           # flash_guard.sh is mandatory
cp ~/.ssh/akam.pub "$SD/akam/authorized_keys"
touch "$SD/AKAM_MAINTENANCE"               # UART route only; the other routes use the first-boot card (see below)
sync
```

Shortcut (this is what `./akam prepare-flash-sd` does): on a card already prepared with
`tools/sd/sd_card.sh` (backup or first-boot card), the flash files are added in one go, with the image name, the 5 MiB size of `mtdblock7.bin`, one image only
and no `*.bin` at the root all checked, then verified:

```bash
tools/sd/sd_card.sh prepare --mode flash-files --device /dev/sdX \
    --image ~/akam-work/out/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs --flash-tool src/ak_rtsp/flash_tool --backup-dir ~/akam-backup
tools/sd/sd_card.sh verify --mode flash-files --device /dev/sdX
```

The manual commands above remain valid. This layout is for the UART route. For the telnet routes use the card described in
[First flash without UART](#first-flash-without-uart-from-the-stock-firmware): do not create
`AKAM_MAINTENANCE` by hand, and do not boot Akam for the first time with the toolkit files on the card.

The script uses fixed paths on the camera: `/mnt/flash_tool` and
`/mnt/custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` (the lexicographically last match is
used, so keep only one image there). It refuses an image that is not exactly 5 242 880 bytes.

## 4. Flash

Three routes, from the least to the most hardware:

| Route | Needs | Used by | Status |
|---|---|---|---|
| [Maintenance telnet](#first-flash-without-uart-from-the-stock-firmware) on a stock camera (first install), then [from the Akam maintenance telnet](#reflashing-over-the-maintenance-telnet-validated-on-hardware) | the flash card, no UART | `./akam flash` | validated on hardware |
| [SSH from RAM](#reflashing-over-ssh-without-uart-ram-only-residual-risk) | a camera already running Akam, no card | `./akam update`, `./akam flash` | validated on hardware, residual risk |
| UART `init=/bin/sh` shell (below) | a UART adapter | by hand only | the safety net |

### UART route

Get a root shell over the **UART console** with the `init=/bin/sh` boot
([hardware_and_uart.md](hardware_and_uart.md#6-dropping-into-a-u-boot-root-shell)). Upstream reports
`flash_tool` working live over Telnet with Wi-Fi and ISP active, but Akam recommends the bare shell
for two reasons: it is the state upstream's restore script was always run in, and on a running camera
the APP partition is mounted on `/usr`, so a live flash erases the filesystem that running programs
(including Dropbear and the stock watchdog) are paged from *(a live flash of an Akam camera survived twice, but it remains a risk)*; see
"Reflashing later" below.

```sh
mount -t proc proc /proc
mount -t vfat /dev/mmcblk0p1 /mnt
ls -l /mnt/custom_firmware /mnt/flash_tool
sh /mnt/install_with_flash_tool.sh
```

The script prints the image `md5sum` (compare it with the PC value), asks you to type `YES` (the wizard sends it only after your own yes), then runs
`flash_tool <image> /dev/mtd7`.

### Verify before you touch the power

The output must end with:

```text
FAULT SUMMARY: 0/1280 chunks needed retry, 0 total failed verify attempts.
Flashing and verification completed successfully!
```

(`flash_tool` prints the exact counts; any "failed" figure other than 0 deserves attention, and any
error line means **do not power cycle**.) As an extra check that Akam adds (not part of the upstream
script), read the partition back and compare with the PC value:

```sh
dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum
```

The result must equal the image's `md5sum`. If verification fails, re-run the install script; if it
still fails, restore your backup (section 5) **before** powering off.

Then power cycle **by hand**: unplug and replug USB-C. The software `reboot` command is unreliable on
this hardware.

After the boot, continue with the [test plan](akam_v1_test_plan.md#2-post-flash-procedure) and the
[SSH guide](akam_v1_ssh_and_lockdown.md#8-test-order-keep-a-way-back-until-ssh-is-proven).

### First flash without UART (from the stock firmware)

This is the variant for a camera that has only been opened through the microSD toolkit shell of the
[SD backup guide](akam_v1_backup_sd.md); it is what `./akam flash` does on a stock camera, and it was
validated on hardware. The flashing itself is the upstream path (`install_with_flash_tool.sh` run live
over the toolkit's Telnet on **stock** firmware), so the stock `/usr` (APP) is mounted and in use while it
is erased: if the write dies half-way, only UART or an SPI programmer recovers the APP.

**1. Card for the flash** (the toolkit card from the backup, with its hook files left as they are, so
that the camera boots stock and opens `telnet CAMERA_IP 24`). Add, on the PC, in these exact places:

| On the card | What |
|---|---|
| `custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` | the one image to flash (keep only one) |
| `flash_tool` | `src/ak_rtsp/flash_tool` (built by `make -C src/ak_rtsp flash_tool`) |
| `install_with_flash_tool.sh`, `flash_guard.sh` | from `tools/flash_scripts/`; `flash_guard.sh` is mandatory, a missing file makes the script refuse |
| `original_firmware/mtdblock7.bin` | your stock APP backup, **in this subdirectory only** (section 3) |
| `restore.sh` | optional, from `tools/flash_scripts/` |
| `preflight_check.sh` | from `tools/flash_scripts/`: read-only Wi-Fi / AP-mode gates (step 3); `--ensure-wifi` creates `/etc/config/wpa_supplicant.conf` from the card's `wifi.conf` when it is missing |

**Never leave any `*.bin` at the top level of the card**: the stock boot runs `system_upgrade` on it
(see the warning in the SD backup guide).

**2. Shell and mount.** Boot the stock camera with this card, wait for the toolkit, then
`telnet CAMERA_IP 24`. The stock boot has already unmounted `/mnt` (lazy `umount`), but every Akam
script reads `/mnt/...`, so mount the card there yourself:

```sh
mount -t vfat /dev/mmcblk0p1 /mnt
ls -l /mnt/custom_firmware /mnt/flash_tool /mnt/flash_guard.sh
```

`flash_guard.sh` passes in this shell: it is not SSH, and the scripts and `flash_tool` live under
`/mnt`, not `/usr`. Do not use `AKAM_FLASH_I_KNOW=1` here.

**3. Gates before writing** (each one costs nothing and an unmet one strands the camera, because there
is no UART to fix it afterwards):

```sh
sh /mnt/preflight_check.sh      # read-only; prints presence and counts, never the SSID or the key
sh /mnt/preflight_check.sh --ensure-wifi   # no saved Wi-Fi? creates it from the card (see troubleshooting)
ls -l /etc/config/wpa_supplicant.conf /etc/config/passwd
```

Will the camera rejoin your Wi-Fi after the flash? Only if `/etc/config/wpa_supplicant.conf` exists:
the wizard creates it when missing (`--ensure-wifi` above), and a boot without a card with Wi-Fi saved was
validated on hardware. What the real dump shows (CONFIG `mtdblock6` and the stock APP scripts, read on the PC): `wpa_supplicant.conf`
exists (223 bytes, one `network` block with an `ssid` and a `psk`, `key_mgmt` list including
`WPA-PSK`); `ethernet.sh` (run by `service.sh`, not by `anyka_ipc`) starts `wpa_supplicant -c
/etc/config/wpa_supplicant.conf` and DHCP (`dhcp=1`) in **station** mode, and goes to AP mode only if
`/tmp/_ht_ap_mode.conf` exists, which `service.sh` copies from `_ht_ap_mode.conf` on the SD card. The
toolkit cards carry that file (it is how their hook runs), so a boot **with** such a card takes the
AP branch and Wi-Fi is then brought up by the toolkit; a boot **without** a card, or with a card
lacking that file, is station mode from `wpa_supplicant.conf`. Akam's patched `service.sh` only
copies it when `AKAM_MAINTENANCE` is present. `hostapd.conf` in CONFIG is empty. The only thing that
rewrites or deletes the file is `recover_cfg.sh` (factory reset, run when the settings file is under
512 bytes; it is 3010 bytes here) and Tuya pairing inside `anyka_ipc`, which Akam removes. Akam's flash does
not touch CONFIG. The router must still accept the saved credentials (same SSID, 2.4 GHz).
- `/etc/config/wpa_supplicant.conf` must exist and be non-empty. Akam removes the Tuya agent that
  used to write it, and `ak_rtsp_wrapper.sh` only starts Wi-Fi when this file exists. The SD backup
  route does **not** write it (the toolkit keeps the Wi-Fi settings in RAM). If it is missing, the
  camera will boot Akam without a network. Create it first, from the same shell, with the block
  below (the wizard's `--ensure-wifi` writes the same format, validated on hardware; this is a
  deliberate write to CONFIG).
- `/etc/config/passwd` should exist (the stock `/etc/passwd` is a symlink to it). If it does not,
  `akam_ssh.sh` creates an **empty** one in CONFIG so that it can bind-mount its private copy over it.
- `cat /proc/mtd` must show the `mtd7: 00500000 ... "APP"` line, and the image must be 5 242 880 bytes
  (the script checks).

Block for a missing `wpa_supplicant.conf` (paste with your SSID and passphrase):

```sh
umask 077
cat > /etc/config/wpa_supplicant.conf <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=1
network={
    ssid="YOUR-SSID"
    psk="YOUR-PASSPHRASE"
    key_mgmt=WPA-PSK
}
EOF
sync
```

**4. Flash and verify** (as in the UART variant, from this shell):

```sh
sh /mnt/install_with_flash_tool.sh
dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum      # must equal the md5sum printed by the script
```

Read the `FAULT SUMMARY` and the success line in full. On any error, **do not power cycle**: rerun
the script; if it still fails, go to "Without UART: what can be recovered" in section 5.

**5. Prepare the first Akam boot (recommended: the "first-boot" card).** Power off by hand (unplug
USB-C) and swap the card for a **second, dedicated card** prepared on the PC (the wizard does this in `prepare-flash-sd`) with
`tools/sd/prepare_first_boot_sd.sh`:

```bash
bash tools/sd/prepare_first_boot_sd.sh --device /dev/sdX --pubkey ~/.ssh/akam.pub
```

The same card can be made with the newer, more general tool (same result, plus a read-only check):

```bash
tools/sd/sd_card.sh prepare --mode first-boot --device /dev/sdX --pubkey ~/.ssh/akam.pub
tools/sd/sd_card.sh verify  --mode first-boot --device /dev/sdX
```

The script verifies the pinned `lsc-tuya-toolkit` commit (`396db8a`) and the `sd_card` tree sha256
(`3aa3f698...757747c2f`), refuses any device that is not a whole, removable, USB disk, shows `lsblk`
and needs a typed `YES` before wiping, waits for the reader to come back after `wipefs` (some
readers report "No medium found"), asks for the SSID and password **visibly**, and writes the toolkit
payload configured as for the backup (telnet 24, `OFFLINE=1`, `wifi.conf`, entrypoint lock,
`hack.sh` hook, `dump.done`) **plus** an empty `AKAM_MAINTENANCE` and `akam/authorized_keys`. It never
writes a `*.bin` to the card root. `--dry-run --stage-dir DIR` builds the tree without touching a
device. The card holds your Wi-Fi password in clear text: keep it private.

Why this works: with the card in, the patched `service.sh` lets the toolkit hook run (flag present) and
records `/tmp/akam_maintenance` while the SD is still mounted, so `akam_lockdown.sh` does not kill the
toolkit's telnet on port 24 after the stock script unmounts the card, while `akam_ssh.sh` brings up
SSH on port 22 from `akam/authorized_keys`. Power on, wait a few minutes, then:

1. `ssh -i ~/.ssh/akam root@CAMERA_IP` and run `akam_hwcheck.sh` (it warns about maintenance mode).
2. Once SSH is validated, **unplug USB-C, remove the card, and boot again**: maintenance mode lasts
   only until the next boot without the flag, so telnet 24 must be gone (ports 21, 23, 24 closed) and
   SSH must still work with the same host fingerprint.
3. Keep the first-boot card for emergencies only if you accept that it re-opens an unauthenticated
   telnet whenever it is inserted; erase it otherwise.

If SSH does not come up but telnet 24 does, you have a root shell
to investigate (Wi-Fi, `/tmp/akam_ssh.log`) before touching anything else.

### Reflashing later: not from a plain SSH session

Do **not** re-flash a running Akam camera by running the flash script from an ordinary SSH login (the SSH
route below is different: it runs from RAM). On a running Akam camera the APP
partition is mounted on `/usr`, and everything Akam starts lives there: `dropbear` and `dropbearkey`
(`/usr/bin`), `ak_rtsp_wrapper.sh`, `akam_ssh.sh`, `akam_lockdown.sh`, and the stock
`/usr/bin/watchdog` that supervises the system. `flash_tool` erases `/dev/mtd7` in place while those
programs are paged in from it. Expected consequences *(reasoned from the layout and from
`quiet_and_flash_test.sh`, which exists because killing the watchdog stops the hardware timer from
being fed; not tested)*:

- the SSH session, or the shell running the flash, can die in the middle of the write;
- `watchdog` or another process can fault when its pages are read back from a half-erased partition,
  and the hardware watchdog can then reset the board mid-write.

A partly written APP is recoverable only through UART (section 5), so use UART for every re-flash:

1. Attach UART and boot the `init=/bin/sh` shell as in section 4. APP is expected not to be mounted
   there (`rc.local` does not run), so nothing is executing from the partition being written.
2. Run `sh /mnt/install_with_flash_tool.sh` **from the SD card** (`/mnt`), as above. Both the script and
   `flash_tool` are read from the card, never from `/usr`.

UART is the only route with a safety net. Two no-UART routes are described below: the reflash over
the **maintenance telnet** (card in the camera) and the **reflash over SSH from RAM** (no card; residual
risk). Both were validated on hardware. The `make -C src/ak_rtsp upload`
target copies `install_with_flash_tool.sh` and `quiet_and_flash_test.sh` into `/tmp` but does not flash;
do not run those two from an SSH session on an Akam image (the guard refuses them).

**`flash_guard.sh`.** Every script that writes APP sources `flash_guard.sh` from its own directory, so
copy it to the SD card next to the scripts (a missing file makes the script refuse to run). The guard
has two entry points:

- `akam_flash_guard` (all the install/restore scripts): refuses inside an SSH session
  (`SSH_CONNECTION`/`SSH_CLIENT`, or a dropbear ancestor process) and when the script or `flash_tool`
  lives under `/usr` while `/usr` is the `mtdblock7` squashfs. It does not block a flash from the stock
  firmware over Telnet, the maintenance telnet, or the `init=/bin/sh` shell, with scripts and
  `flash_tool` on the SD card. `AKAM_FLASH_I_KNOW=1` overrides it (prints a warning; the former name `CAMYN_FLASH_I_KNOW=1` is a
  deprecated alias, accepted with an extra warning for one release); use it only on
  **stock firmware**, never on an Akam image.
- `akam_flash_guard_detached` (only `akam_reflash_ssh.sh`, for the SSH route below): no override
  variable. It allows the write only if **all** hold: the operator passed exactly
  `--i-understand-no-uart`; the script, `flash_tool` and the image all resolve (symlinks followed)
  under `/tmp`, none under `/usr`, and `/tmp` is a tmpfs/ramfs mount; `SSH_CONNECTION`, `SSH_CLIENT`
  and `SSH_TTY` are unset; no dropbear process is among the ancestors; and the process is a session
  leader (started with `setsid`), so the death of the SSH session cannot signal it. The old rule is
  unchanged: running the same write directly from an SSH shell, or with tools under `/usr`, is still
  refused.

### Reflashing over the maintenance telnet (validated on hardware)

This is the procedure that was run on real Akam cameras (first from the stock firmware's telnet, then from
the Akam maintenance telnet, 2026-11-08 onwards). Each time `flash_tool` needed **0 retries**,
the read-back md5 of `/dev/mtd7` matched the image, and the running system (including the stock
`/usr/bin/watchdog`, which lives in the APP) survived the write. That is an observation on one unit,
not a guarantee: see the residual risks of the SSH route, which apply here too.

1. Card: the first-boot card from `tools/sd/prepare_first_boot_sd.sh` (toolkit payload, empty
   `AKAM_MAINTENANCE`, `akam/authorized_keys`), see the tips below. Boot with it and
   `telnet CAMERA_IP 24`.
2. The maintenance boot mounts the card on **`/tmp/sd`**, not on `/mnt` (the stock script unmounts
   `/mnt`). Every Akam script reads `/mnt/...`, so bind the card there:
   ```sh
   mount --bind /tmp/sd /mnt
   ls -l /mnt/custom_firmware /mnt/flash_tool /mnt/flash_guard.sh /mnt/install_with_flash_tool.sh
   ```
3. If the image or `flash_tool` on the card are not the ones you want, do not pull the card: **push**
   the new files from the PC. The PC's firewall blocks inbound connections, so the camera cannot
   fetch them; it listens instead and the PC connects. On the camera (one file per run, into the card's
   tree that you just bound on `/mnt`):
   ```sh
   nc -l -p 8765 > /mnt/custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs
   ```
   and, once it is listening, on the PC (tiny socket client, no netcat needed):
   ```sh
   python3 -c 'import socket,sys; s=socket.create_connection((sys.argv[1],8765)); s.sendall(open(sys.argv[2],"rb").read()); s.close()' CAMERA_IP ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs
   ```
   `nc -l` exits when the PC closes the connection. Check the result on the camera
   (`ls -l`, `md5sum`) against the PC value before continuing; repeat for `flash_tool`
   (`nc -l -p 8765 > /mnt/flash_tool; chmod +x /mnt/flash_tool`) and for any script you changed.
   Keep exactly one `ak_rtsp_firmware_*.squashfs` in `custom_firmware/`, and never a `*.bin` at the root.
4. `sh /mnt/install_with_flash_tool.sh`: compare the printed `md5sum` with the PC, type `YES`, read the
   `FAULT SUMMARY` and the success line in full, then
   `dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum` must equal the image md5. The guard accepts this: the
   shell is not SSH and everything runs from `/mnt`.
5. Power cycle by hand (unplug and replug USB-C), then follow the post-flash checks.

### Reflashing over SSH, without UART (RAM-only, residual risk)

For a camera with no UART and no card handy. **Read the risks first.** The design removes the two
dependencies the plain "SSH reflash" has (the SSH session and the tools live on the APP partition being
erased), but it cannot make the write itself safe:

- **Residual risk 1, no safety net.** If power fails or the board resets while `/dev/mtd7` is half
  written, the APP does not boot and nothing on the SD card runs (section 5, "Without UART"). Only UART
  (`init=/bin/sh`) or an SPI programmer recovers it.
- **Residual risk 2, `/usr` is still the partition being rewritten, and the stock watchdog runs from
  it.** `flash_tool` erases the whole range first and writes afterwards, so for the whole write any
  `/usr` page that is not already in the page cache reads back as blank flash or as the new image.
  What the stock `/usr/bin/watchdog` does (read from the extracted APP, disassembled, not run under a
  debugger): its main loop wakes about once per second (`CMsgClient::waitMsg`, 1 s limit) and each time
  calls `clearHardwareWatchdog`, an `ioctl(WDIOC_KEEPALIVE)` on `/dev/watchdog`, then
  `respawnProcessList` and `checkWatchdog`. Nothing else on an Akam image feeds the hardware timer
  (`ak_rtsp` and the wrapper never open the device). The kernel driver (`ak_top_wdt`) is a nowayout
  one ("Device closed unexpectedly - timer will not stop"), so if the daemon dies the timer keeps
  counting and the board resets when it expires. The timeout was not read from the kernel; it is longer
  than the 5 s feeding period of `start_rtsp.sh`. What follows:
  - The pages of that loop (the daemon and `libcommon.so`) are touched every second, so they stay in the
    cache unless RAM runs out (about 29 MB free before staging, roughly 10 MB less after). That is the likely
    reason the live flashes survived.
  - If the daemon takes a fault (a page evicted, then re-read from erased flash: SIGBUS) the hardware
    resets the board within the timeout, in the middle of the write, and only UART recovers it.
  - Cold paths also read `/usr`: a respawned child (`/usr/bin/ak_rtsp_wrapper.sh`) and
    `akam_lockdown.sh`, which runs `/usr/sbin/akam_ssh.sh start` every 60 s. They can fail or run
    garbage; that hurts that child, not the write.
  - A few successes prove little: with no failure in two trials the 95 % upper bound on the failure rate
    is still about 78 %. Prefer the card route, and treat this one as a risk you accept deliberately.
  - **Mitigation evaluated, not implemented:** kill the daemon with `SIGKILL` and feed `/dev/watchdog`
    from a RAM BusyBox (`while :; do printf V; sleep 5; done > /dev/watchdog`, the pattern of
    `start_rtsp.sh` and `quiet_and_flash_test.sh`). The device is exclusive-open, so the daemon cannot
    be fed around; it has to die first, and `start_rtsp.sh` documents an `EBUSY` race when the feeder
    opens the device too early. The timer keeps running between the kill and the first feed, with an
    unknown timeout. It also removes the supervisor (nothing would respawn the wrapper). It has never
    run on an Akam image, whereas the unmodified path survived twice, so adding it blind to a path with
    no safety net was judged worse than documenting the risk. Try it first with UART connected.
- **Residual risk 3, RAM.** The image (5 MiB), `flash_tool`, a BusyBox copy and a Dropbear copy go to
  tmpfs; with ~29 MB free the script requires 10 MiB free in `/tmp` and aborts otherwise.

What the scripts do:

1. `reflash_over_ssh.sh` (PC) checks the image (exactly 5 242 880 bytes, md5 equal to the one **you**
   pass), uploads `akam_reflash_ssh.sh`, `flash_guard.sh`, `flash_tool` and the image to
   `/tmp/akam_reflash/` with `ssh ... 'cat > file'`, and compares the sha256 computed on the PC and on
   the camera for the image and the tool.
2. On the camera, `precheck` requires `/proc/mtd` to show `mtd7: 00500000 ... "APP"` and enough free
   `/tmp`; `prepare` re-checks size and md5, copies BusyBox into RAM (linking the applets the write
   needs), copies `dropbearmulti` to `/tmp` and starts a **second Dropbear on port 2222** from there,
   with the same host key and the same key directory (`/tmp/akam_home`) as the first one, so you have
   a session that does not execute anything from `/usr`. Because it is the same host key,
   the helper passes `-o HostKeyAlias=<host>` and your `known_hosts` entry applies to port 2222.
3. The helper logs in on port 2222. If it cannot, it stops **before writing**.
4. Through that session, `start --i-understand-no-uart` launches a worker with `setsid`, from the RAM
   copies, with `SSH_*` removed from its environment, stdin on `/dev/null` and stdout/stderr on
   `/tmp/akam_reflash/flash.log`. The worker waits until the launching shell is gone, runs
   `akam_flash_guard_detached` (rules above) and refuses (`RESULT: REFUSED`, nothing written) otherwise;
   then it takes an atomic lock (`/tmp/akam_reflash/worker.lock`, so a second worker or a retried
   `start` cannot write at the same time), checks the md5 and exact size of the RAM image and that
   `/proc/mtd` still shows `mtd7` as the 5 MiB `APP`, runs `flash_tool image /dev/mtd7`, reads the
   partition back and compares its md5, up to 3 attempts. The log ends with `RESULT: OK`,
   `RESULT: FAILED` or `RESULT: REFUSED` (nothing written). A dying SSH session cannot kill it.
5. The helper polls the log over port 2222 (tolerating dropped connections), prints the new lines, and
   when it sees `RESULT: OK` runs its own read-back (`dd if=/dev/mtd7 | md5sum` in a fresh session) and
   compares it with the md5 you gave. Only then does it print that the flash is verified and tell you
   to power-cycle **by hand**. Nothing in these scripts reboots the camera.

Run it (the `flash_tool` is the ARM binary from `make -C src/ak_rtsp flash_tool`):

```bash
# 1. dry run: uploads only the two scripts, checks mtd7 and free RAM, writes nothing
tools/flash_scripts/reflash_over_ssh.sh --host CAMERA_IP --identity ~/.ssh/akam \
    --image build/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs --md5 <md5 of the image> \
    --flash-tool src/ak_rtsp/flash_tool --precheck-only
# 2. the flash itself
tools/flash_scripts/reflash_over_ssh.sh ... same options ... --i-understand-no-uart
```

If the helper ends with anything but "VERIFIED", **do not power cycle**: read
`/tmp/akam_reflash/flash.log` over port 2222, run the helper again (while a worker is running the
helper refuses before touching anything, and `start` refuses too; after a finished or dead worker it
replaces only the files it uploads), or go to UART. If the session on port 22 drops while you wait, nothing is lost:
the worker is detached; reconnect on port 2222.

### Maintenance-card tips

- The maintenance boot mounts the card on `/tmp/sd`; `mount --bind /tmp/sd /mnt` makes the unmodified
  scripts work. The stock boot unmounts `/mnt` lazily, so do not expect it there.
- The card is the trust anchor: whoever can insert it gets an unauthenticated root telnet on port 24.
  Keep it for flashing days only, and erase or remove `AKAM_MAINTENANCE` afterwards.
- Never leave a `*.bin` at the card root (the stock boot would try to flash it). Keep the stock backup in
  `original_firmware/`, and exactly one image in `custom_firmware/`.
- Put `akam/authorized_keys` on the card: it is the way back into SSH after a flash.
- The camera's BusyBox 1.30.1 has `setsid`, `nc`, `timeout`, `sha256sum`, `md5sum`, `stat`, `cmp`,
  `grep -E`, `strings`, `paste`, `cut`, `sed`, `wc`, and **lacks** `tr`, `mktemp`, `nohup`, `flock`,
  `gzip`, `uniq`, `wget`. Do not write procedures that need them.
- Moving files: the PC cannot be reached from the camera (firewall), so the camera listens
  (`nc -l -p 8765 > file`) and the PC pushes; the camera has no `wget`.
- `/tmp` is a tmpfs of ~22 MB free and total free RAM is ~29 MB: do not stage more than one image.
- The software `reboot` is unreliable on this hardware: always unplug and replug USB-C.

## 5. Restore

### Back to the stock APP

Needs `original_firmware/mtdblock7.bin` on the card (section 3). From the UART `init=/bin/sh` shell
(upstream states `restore.sh` has only ever been run that way):

```sh
mount -t proc proc /proc
mount -t vfat /dev/mmcblk0p1 /mnt
sh /mnt/restore.sh
```

It checks the file is exactly 5 242 880 bytes, asks for `YES`, and writes `/dev/mtdblock7` with `dd`
(upstream script; note the DMA warning in section 2: this is acceptable only in the bare shell where
Wi-Fi and ISP drivers are idle). `flash_tool /mnt/original_firmware/mtdblock7.bin /dev/mtd7` is an
alternative that uses the verified-write tool; the repository's restore scripts do not use it, so
treat it as untested. Verify afterwards with `dd if=/dev/mtd7 bs=64k | md5sum` against the hash of
your backup, then power cycle by hand.

Restoring the APP leaves CONFIG alone, so the `dropbear` directory and Wi-Fi settings stay. That is
harmless for stock.

Go assistant: `akam update --stock` (or "Stock firmware (from your backup)" in the "Which firmware?" list of
"Update this camera") runs the same restore command with the same gates (a backup of the stock firmware re-hashed now, the
red box, default No). Its route question defaults to No: the maintenance telnet and the card; answer yes to restore over SSH
when Akam runs. It has never been tried on a real camera
([choose your firmware](wizard_guide.md#choosing-your-firmware-go-assistant-only)).

### Back to full stock

`tools/flash_scripts/restore_full_firmware.sh` rewrites **all 8 partitions**, including UBOOT, from the
backup. It is a last resort: a power loss or error while the bootloader is being rewritten is the most
likely way to hard-brick the camera. Akam never needs it unless something other than APP was damaged.

### Recovery when the APP is bad (no network, no SSH)

Akam rebuilds only APP, so the bootloader, kernel and ROOTFS are untouched and U-Boot should stay
reachable:

1. Attach UART (115200 8N1, 3.3 V) and interrupt U-Boot at power-on (the ESP32 bridge helps; the
   window is about one second: [hardware_and_uart.md](hardware_and_uart.md#5-automated-u-boot-interception--bridge)).
2. Boot the `init=/bin/sh` root shell as in [section 6 of that guide](hardware_and_uart.md#6-dropping-into-a-u-boot-root-shell).
   It uses `root=/dev/mtdblock5` (ROOTFS), which does not depend on APP.
3. Mount `/proc` and the SD card, then run `restore.sh` as above.

A card holding `original_firmware/` and the scripts is therefore your out-of-band recovery medium;
the UART is the other half.

### Without UART: what can be recovered

The SD-card hooks (`_ht_ap_mode.conf` + `hostapd`, `AKAM_MAINTENANCE`, the `*.bin` upgrade path) are
implemented by `sbin/service.sh` **inside the APP partition**, so they exist only while a readable APP
is in place. Consequences, stated plainly:

| Situation after the flash | Recovery without UART |
|---|---|
| The write reported an error, the camera is still running | Rerun `install_with_flash_tool.sh`, or run `restore.sh` (stock APP) from the same shell. Do not power cycle before one of them succeeds. |
| The APP image is corrupt or does not mount (no boot to `service.sh`) | **None.** Nothing on the SD card runs. UART (`init=/bin/sh`, section 5 below) or an external SPI programmer is required. |
| The APP boots but there is no network (no Wi-Fi configuration, driver trouble) | **None from the network.** `AKAM_MAINTENANCE` re-enables the card's hook in `service.sh` and, through the RAM marker `/tmp/akam_maintenance`, makes `akam_lockdown.sh` stand down until the next reboot without the flag, so a toolkit-style telnet hook is no longer killed. This path is covered by host tests, **not yet proven on hardware**, and it needs a working network to be of any use. |
| SSH refused (key not on the card, wrong key) | Put the right `akam/authorized_keys` on the card and power cycle (keys are re-read at every boot). Needs the camera on the network. |
| `*.bin` OTA-style upgrade from the card | Allowed only with `AKAM_MAINTENANCE`; Akam does not provide an OTA-format file, so this is not a recovery path. |

So the no-UART first flash is a bet that the image mounts and the network comes up. The image checks
(`build_firmware.sh` round-trip, padding, exact size) and the pre-flash gates above reduce but do not
remove that risk; the xz SquashFS format is the stock one (the running kernel already mounts xz ROOTFS and APP), but Akam's own image has not been booted.

### Caveat: restore is not guaranteed

These procedures reuse upstream scripts that have worked for upstream, but:

- they depend on U-Boot, the SPI NOR and power staying healthy during the write;
- an interrupted or corrupted erase/write of the bootloader area, a failing flash chip, or a flash that
  does not accept writes leaves the camera unbootable;
- in that case the only remaining option is an external SPI programmer clipped or soldered to the
  flash chip (for example a CH341A) written with your backup.

Akam has not rehearsed a restore on any camera. Whether you rehearse one before relying on it is
your decision; a rehearsal is itself a flash write.

## 6. Checklist

- [ ] Backup verified and copied off the SD card ([backup guide](akam_v1_backup.md)).
- [ ] UART attached and a root shell obtained before flashing, **or** the no-UART risks above accepted
      and the pre-flash gates (`wpa_supplicant.conf`, `passwd`, `/proc/mtd`) passed.
- [ ] Image hash checked on PC; same `md5sum` on the camera.
- [ ] Only `/dev/mtd7` written; verification summary read in full.
- [ ] Read-back `md5sum` equals the image.
- [ ] Power cycled by hand; card with `akam/authorized_keys` inserted.
