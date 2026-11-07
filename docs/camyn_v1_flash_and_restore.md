# Camyn V1: Flashing and Restoring

> **RISK.** Writing the SPI NOR can brick the camera. Restoring is **not guaranteed** to work (section
> 5). Do not start without a verified 8-partition backup ([backup guide](camyn_v1_backup.md)). A UART
> connection ([hardware_and_uart.md](hardware_and_uart.md)) is the only way back from an APP that does
> not boot; the [no-UART variant](#first-flash-without-uart-from-the-stock-firmware) below exists, but
> read what it can and cannot recover (section 5) before choosing it.
>
> Status: nothing in this document has been run on a camera by Camyn. The flashing tool and scripts
> (`flash_tool`, `install_with_flash_tool.sh`, `restore.sh`) are upstream's; upstream reports a gzip APP
> image flashed and booted on its own unit (`3215672.2`). Camyn's image, and every step that is
> Camyn's own addition, is untested.

---

## 1. Why Camyn's own procedure and not the flasher

The [Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) (v0.3.2) is
a good tool, but for the first Camyn camera the decision (O-2c) is **not to use it**:

- Its repository and commit constants (`FIRMWARE_REPO`, `FIRMWARE_COMMIT`) are hard-coded to the upstream
  project. It builds upstream's `ak_rtsp`, not this tree, and knows nothing about Dropbear, the SSH
  boot script, or the lockdown patch.
- It patches `ak_rtsp` sources on the fly. Its fourth patch (the `night.c` day/night override) is
  expected to **fail on this source tree**, where the flasher's fixes are already integrated
  (see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).
- It talks to the camera through an unauthenticated Telnet shell on port 24, opened by an SD-card hook,
  which is exactly what Camyn closes. A card left over from the flasher keeps that hook.
- The flasher's own post-flash check does not look at SSH.

Camyn's procedure is: manual backup, `build_firmware.sh`, then `install_with_flash_tool.sh` below. The
flasher stays a reference. Adapting it to Camyn is deferred until Camyn has been validated on hardware.
The image builder still honours the flasher's interface (`APP_EXTRACTED`, `FW_DIR`, output name,
exact size).

## 2. What is written, and what never is

- Only `/dev/mtd7` (APP, 5 242 880 bytes at `0x300000`) is written by the install script.
- **UBOOT, ENV, ENVBK, DTB, KERNEL and ROOTFS are never written by Camyn V1.** CONFIG is only written
  by the camera itself at runtime (Dropbear keys).
- Do not use `dd` to write `/dev/mtdblock7` on a running system: upstream documents silent corruption
  from SPI-NOR DMA contention ([details](firmware_building_and_flashing.md#5-the-spi-nor-dma-contention-gotcha)).
  `flash_tool` writes 4 KiB blocks to the raw `/dev/mtd7` and reads each back, retrying on mismatch.

## 3. Prepare the SD card

On the PC, check the image, then copy files to a FAT32 card:

```bash
cd ~/camyn-work/out
sha256sum -c ak_rtsp_firmware_*.squashfs.sha256
md5sum ak_rtsp_firmware_*.squashfs          # note it; the camera prints md5sum too

SD=/run/media/$USER/CARD                    # adapt
mkdir -p "$SD/custom_firmware" "$SD/camyn" "$SD/original_firmware"
cp ak_rtsp_firmware_*.squashfs "$SD/custom_firmware/"
cp ~/camyn-backup/mtdblock*.bin "$SD/original_firmware/"          # for restore
cp ~/Projects/camyn/src/ak_rtsp/flash_tool "$SD/"                 # path of your checkout
cp ~/Projects/camyn/tools/flash_scripts/install_with_flash_tool.sh \
   ~/Projects/camyn/tools/flash_scripts/restore.sh \
   ~/Projects/camyn/tools/flash_scripts/flash_guard.sh "$SD/"   # flash_guard.sh is mandatory
cp ~/.ssh/camyn.pub "$SD/camyn/authorized_keys"
touch "$SD/CAMYN_MAINTENANCE"               # UART route only (see the SSH guide); NOT for the no-UART route
sync
```

This layout is for the UART route. For the no-UART route use the card described in
[First flash without UART](#first-flash-without-uart-from-the-stock-firmware): do not create
`CAMYN_MAINTENANCE`, and do not boot Camyn for the first time with the toolkit files on the card.

The script uses fixed paths on the camera: `/mnt/flash_tool` and
`/mnt/custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` (the lexicographically last match is
used, so keep only one image there). It refuses an image that is not exactly 5 242 880 bytes.

## 4. Flash

Get a root shell over the **UART console** with the `init=/bin/sh` boot
([hardware_and_uart.md](hardware_and_uart.md#6-dropping-into-a-u-boot-root-shell)). Upstream reports
`flash_tool` working live over Telnet with Wi-Fi and ISP active, but Camyn recommends the bare shell
for two reasons: it is the state upstream's restore script was always run in, and on a running camera
the APP partition is mounted on `/usr`, so a live flash erases the filesystem that running programs
(including Dropbear and the stock watchdog) are paged from *(assumed risk, not tested)*; see
"Reflashing later" below.

```sh
mount -t proc proc /proc
mount -t vfat /dev/mmcblk0p1 /mnt
ls -l /mnt/custom_firmware /mnt/flash_tool
sh /mnt/install_with_flash_tool.sh
```

The script prints the image `md5sum` (compare it with the PC value), asks you to type `YES`, then runs
`flash_tool <image> /dev/mtd7`.

### Verify before you touch the power

The output must end with:

```text
FAULT SUMMARY: 0/1280 chunks needed retry, 0 total failed verify attempts.
Flashing and verification completed successfully!
```

(`flash_tool` prints the exact counts; any "failed" figure other than 0 deserves attention, and any
error line means **do not power cycle**.) As an extra check that Camyn adds (not part of the upstream
script), read the partition back and compare with the PC value:

```sh
dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum
```

The result must equal the image's `md5sum`. If verification fails, re-run the install script; if it
still fails, restore your backup (section 5) **before** powering off.

Then power cycle **by hand**: unplug and replug USB-C. The software `reboot` command is unreliable on
this hardware.

After the boot, continue with the [test plan](camyn_v1_test_plan.md#2-post-flash-procedure) and the
[SSH guide](camyn_v1_ssh_and_lockdown.md#8-test-order-keep-a-way-back-until-ssh-is-proven).

### First flash without UART (from the stock firmware)

This is the variant for a camera that has only been opened through the microSD toolkit shell of the
[SD backup guide](camyn_v1_backup_sd.md). It is **untested by Camyn**; the flashing itself is the
upstream path (`install_with_flash_tool.sh` run live over the toolkit's Telnet on **stock** firmware,
which upstream reports working), so the stock `/usr` (APP) is mounted and in use while it is erased
*(assumed risk: if the write dies half-way, only UART or an SPI programmer recovers the APP)*.

**1. Card for the flash** (the toolkit card from the backup, with its hook files left as they are, so
that the camera boots stock and opens `telnet CAMERA_IP 24`). Add, on the PC, in these exact places:

| On the card | What |
|---|---|
| `custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` | the one image to flash (keep only one) |
| `flash_tool` | `src/ak_rtsp/flash_tool` (built by `make -C src/ak_rtsp flash_tool`) |
| `install_with_flash_tool.sh`, `flash_guard.sh` | from `tools/flash_scripts/`; `flash_guard.sh` is mandatory, a missing file makes the script refuse |
| `original_firmware/mtdblock7.bin` | your stock APP backup, **in this subdirectory only** (section 3) |
| `restore.sh` | optional, from `tools/flash_scripts/` |

**Never leave any `*.bin` at the top level of the card**: the stock boot runs `system_upgrade` on it
(see the warning in the SD backup guide).

**2. Shell and mount.** Boot the stock camera with this card, wait for the toolkit, then
`telnet CAMERA_IP 24`. The stock boot has already unmounted `/mnt` (lazy `umount`), but every Camyn
script reads `/mnt/...`, so mount the card there yourself:

```sh
mount -t vfat /dev/mmcblk0p1 /mnt
ls -l /mnt/custom_firmware /mnt/flash_tool /mnt/flash_guard.sh
```

`flash_guard.sh` passes in this shell: it is not SSH, and the scripts and `flash_tool` live under
`/mnt`, not `/usr`. Do not use `CAMYN_FLASH_I_KNOW=1` here.

**3. Gates before writing** (each one costs nothing and an unmet one strands the camera, because there
is no UART to fix it afterwards):

```sh
ls -l /etc/config/wpa_supplicant.conf /etc/config/passwd
```

- `/etc/config/wpa_supplicant.conf` must exist and be non-empty. Camyn removes the Tuya agent that
  used to write it, and `ak_rtsp_wrapper.sh` only starts Wi-Fi when this file exists. The SD backup
  route does **not** write it (the toolkit keeps the Wi-Fi settings in RAM). If it is missing, the
  camera will boot Camyn without a network. Create it first, from the same shell, with the block
  below *(untested format, matches the `wpa_cli -p/var/run/wpa_supplicant` used by the stock scripts;
  this is a deliberate write to CONFIG)*.
- `/etc/config/passwd` should exist (the stock `/etc/passwd` is a symlink to it). If it does not,
  `camyn_ssh.sh` creates an **empty** one in CONFIG so that it can bind-mount its private copy over it.
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

**5. Prepare the first Camyn boot.** Power off by hand (unplug USB-C), take the card out, and put in
a card with **only** `camyn/authorized_keys` (the exact path the camera reads; see the
[SSH guide](camyn_v1_ssh_and_lockdown.md#2-put-the-public-key-on-the-microsd-card)), **without**
`CAMYN_MAINTENANCE` and without the toolkit files (use a freshly formatted FAT32 card with an MBR
partition). The toolkit card must not be used for the first Camyn boot: with the flag it would revive
the toolkit hook (unauthenticated Telnet on port 24, a second Wi-Fi bring-up), and without the flag
it does nothing useful. Then power on and wait a few minutes.

### Reflashing later: never over an SSH session

Do **not** re-flash a running Camyn camera from an SSH login. On a running Camyn camera the APP
partition is mounted on `/usr`, and everything Camyn starts lives there: `dropbear` and `dropbearkey`
(`/usr/bin`), `ak_rtsp_wrapper.sh`, `camyn_ssh.sh`, `camyn_lockdown.sh`, and the stock
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

If you have no UART, the only variant that avoids running from `/usr` is a fully detached run of the
flash tool from RAM and SD (`nohup`, output to a file, copy of `flash_tool` in `/tmp`, shell and `dd`
resolved from ROOTFS). Camyn has **not** tried this and does not recommend it: you still lose the
watchdog feeder and the SSH session, and cannot see the result. The `make -C src/ak_rtsp upload` target
copies `install_with_flash_tool.sh` and `quiet_and_flash_test.sh` into `/tmp` but does not flash; do
not run them from an SSH session on a Camyn image.

**`flash_guard.sh`.** Every script that writes APP sources `flash_guard.sh` from its own directory, so
copy it to the SD card next to the scripts (a missing file makes the script refuse to run). The guard
refuses inside an SSH session (`SSH_CONNECTION`/`SSH_CLIENT`, or a dropbear ancestor process) and when
the script or `flash_tool` lives under `/usr` while `/usr` is the `mtdblock7` squashfs. It does not
block the first flash from stock firmware over Telnet/UART or the `init=/bin/sh` shell, with scripts
and `flash_tool` on the SD card. `CAMYN_FLASH_I_KNOW=1` overrides it (prints a warning); use it only
on **stock firmware**, never on a Camyn image.

Note that the header comments of `install_with_flash_tool.sh` and `quiet_and_flash_test.sh` describe
live flashing over **Telnet** on the stock firmware (a shell that is not Dropbear). That experience
does not transfer to a Camyn camera, where Telnet is disabled and the session would be SSH.

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

### Back to full stock

`tools/flash_scripts/restore_full_firmware.sh` rewrites **all 8 partitions**, including UBOOT, from the
backup. It is a last resort: a power loss or error while the bootloader is being rewritten is the most
likely way to hard-brick the camera. Camyn never needs it unless something other than APP was damaged.

### Recovery when the APP is bad (no network, no SSH)

Camyn rebuilds only APP, so the bootloader, kernel and ROOTFS are untouched and U-Boot should stay
reachable:

1. Attach UART (115200 8N1, 3.3 V) and interrupt U-Boot at power-on (the ESP32 bridge helps; the
   window is about one second: [hardware_and_uart.md](hardware_and_uart.md#5-automated-u-boot-interception--bridge)).
2. Boot the `init=/bin/sh` root shell as in [section 6 of that guide](hardware_and_uart.md#6-dropping-into-a-u-boot-root-shell).
   It uses `root=/dev/mtdblock5` (ROOTFS), which does not depend on APP.
3. Mount `/proc` and the SD card, then run `restore.sh` as above.

A card holding `original_firmware/` and the scripts is therefore your out-of-band recovery medium;
the UART is the other half.

### Without UART: what can be recovered

The SD-card hooks (`_ht_ap_mode.conf` + `hostapd`, `CAMYN_MAINTENANCE`, the `*.bin` upgrade path) are
implemented by `sbin/service.sh` **inside the APP partition**, so they exist only while a readable APP
is in place. Consequences, stated plainly:

| Situation after the flash | Recovery without UART |
|---|---|
| The write reported an error, the camera is still running | Rerun `install_with_flash_tool.sh`, or run `restore.sh` (stock APP) from the same shell. Do not power cycle before one of them succeeds. |
| The APP image is corrupt or does not mount (no boot to `service.sh`) | **None.** Nothing on the SD card runs. UART (`init=/bin/sh`, section 5 below) or an external SPI programmer is required. |
| The APP boots but there is no network (no Wi-Fi configuration, driver trouble) | **None from the network.** `CAMYN_MAINTENANCE` does re-enable the card's hook in `service.sh`, but `camyn_lockdown.sh` looks for the flag on `/mnt`, which the stock boot has already unmounted, so it keeps killing `telnetd`, `telnet.sh` and `hack.sh` seconds after they start. A custom hook script with other process names might survive; Camyn has not tried it. |
| SSH refused (key not on the card, wrong key) | Put the right `camyn/authorized_keys` on the card and power cycle (keys are re-read at every boot). Needs the camera on the network. |
| `*.bin` OTA-style upgrade from the card | Allowed only with `CAMYN_MAINTENANCE`; Camyn does not provide an OTA-format file, so this is not a recovery path. |

So the no-UART first flash is a bet that the image mounts and the network comes up. The image checks
(`build_firmware.sh` round-trip, padding, exact size) and the pre-flash gates above reduce but do not
remove that risk; the gzip SquashFS format is proven only by upstream's reports, not on this unit.

### Caveat: restore is not guaranteed

These procedures reuse upstream scripts that have worked for upstream, but:

- they depend on U-Boot, the SPI NOR and power staying healthy during the write;
- an interrupted or corrupted erase/write of the bootloader area, a failing flash chip, or a flash that
  does not accept writes leaves the camera unbootable;
- in that case the only remaining option is an external SPI programmer clipped or soldered to the
  flash chip (for example a CH341A) written with your backup.

Camyn has not rehearsed a restore on any camera. Whether you rehearse one before relying on it is
your decision; a rehearsal is itself a flash write.

## 6. Checklist

- [ ] Backup verified and copied off the SD card ([backup guide](camyn_v1_backup.md)).
- [ ] UART attached and a root shell obtained before flashing, **or** the no-UART risks above accepted
      and the pre-flash gates (`wpa_supplicant.conf`, `passwd`, `/proc/mtd`) passed.
- [ ] Image hash checked on PC; same `md5sum` on the camera.
- [ ] Only `/dev/mtd7` written; verification summary read in full.
- [ ] Read-back `md5sum` equals the image.
- [ ] Power cycled by hand; card with `camyn/authorized_keys` inserted.
