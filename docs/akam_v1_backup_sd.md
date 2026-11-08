# Akam V1: Back Up the Camera with a microSD Root Shell (no UART)

This is an alternative to the UART route in [akam_v1_backup.md](akam_v1_backup.md). It gets a
root shell over Wi-Fi by booting the stock firmware from a prepared microSD card. The camera is not
opened and **nothing is flashed**.

> **Status and honesty notes.** Every step is marked with its origin:
>
> - **[Toolkit]** comes from the SD-card toolkit
>   [`tasarren/lsc-tuya-toolkit`](https://github.com/tasarren/lsc-tuya-toolkit) (README and scripts read at the pinned commit below).
> - **[Flasher]** comes from stage 1 of
>   [`4ddict/Action-Smart-Connect-Flasher`](https://github.com/4ddict/Action-Smart-Connect-Flasher) v0.3.2
>   (commit `b99baf2`), which prepares the same toolkit and runs a backup.
> - **[Akam, untested]** was written for Akam and has **not been run on a camera**. This applies to
>   the procedure as a whole: Akam reproduces the flasher's stage 1 by hand, minus its Wi-Fi
>   persistence, and adds a second read plus a hash comparison.
>
> The toolkit was reverse-engineered on firmware `6.2863.123` (it says it "should work on any version,
> as long as you tweak the config files"). The firmware version of your unit is unknown. The flasher
> lists `3215672.3` / SI `D26228` as "reported working by a user" with its wizard 0.3.0, not tested by
> its author. If anything below does not happen as described, stop and fall back to the UART route.

> **WARNING: never boot the camera with `.bin` files on the card.** The stock `service.sh` runs
> `tf_update.sh` at every boot, which mounts the card on `/mnt` and calls `system_upgrade` on every
> file matching `/mnt/*.bin` (top level of the card only; subdirectories such as `dump1/` and
> `dump2/` and hidden files are not matched). `system_upgrade` writes the MTD partitions named in
> the file, so a stray OTA-format `.bin` on any card could reflash the camera. The checks in
> `system_upgrade` (bundle name, higher version) are the only protection. Keep dumps in
> subdirectories, and **remove the card before powering the camera again** once the dump is done.
> Akam images built from this repository guard that call behind `/mnt/AKAM_MAINTENANCE`, but the
> stock firmware you are dumping is not protected.

---

## 1. Why not just run the flasher?

**Do not run `install.sh`** for a backup-only job.

- **[Flasher]** `install.sh` has no option to stop after stage 1. Its only flags are `--help` and
  `--version`. It is one linear wizard: stage 1 (backup) is followed directly by the firmware build,
  the stage 2 instructions and the flash. Quitting at the right prompt is possible but is a manual
  discipline, not a safeguard.
- **[Flasher]** Stage 1 also **writes your Wi-Fi network permanently into the CONFIG flash partition**
  (`/etc/config/wpa_supplicant.conf`). A backup should not change the flash.
- **[Flasher]** It also installs packages, formats the card, downloads a compiler, and so on.

Stage 1 is only file copying onto a FAT32 card plus a camera-side script (`camera/decloud_stage1.sh`),
so it can be reproduced by hand without the persistent-Wi-Fi part. That is what this guide does.

---

## 2. Prerequisites

- Camera: label `3215672.3`, SI `D26228` expected (see [akam_v1_backup.md](akam_v1_backup.md), section 3).
  Write the label down.
- A microSD card. **[Toolkit]** FAT32 is required. **[Flasher]** "any small card is sufficient"; it creates
  one MBR partition of type `c` (FAT32 LBA). Akam needs about 17 MiB free for two dumps (8 MiB each).
  Prefer a small card (up to 32 GB); larger cards were not tested by Akam.
- A 2.4 GHz Wi-Fi network the camera can join with DHCP (**[Flasher]**: the camera is 2.4 GHz only),
  and a PC on the same LAN/VLAN.
- On the Arch PC: `git`, `dosfstools`, `util-linux`, `inetutils` (for `telnet`), `squashfs-tools`, and optionally `nmap`.

```bash
sudo pacman -S --needed git dosfstools util-linux inetutils squashfs-tools nmap
```

The card's contents (Wi-Fi password in clear text) must be treated as secret. See section 11.

---

## 3. Get the pinned toolkit and verify it

**[Flasher]** pins the toolkit to commit `396db8a` (`Do not create dump dirs on each boot`,
2026-02-25). Akam uses the same commit. Download it and **only read and copy files; never execute
anything from it on the PC**.

```bash
mkdir -p ~/akam-sdprep && cd ~/akam-sdprep
git clone https://github.com/tasarren/lsc-tuya-toolkit.git toolkit
git -C toolkit checkout --detach 396db8a835d9787b2997de038b48f645c837e754
git -C toolkit rev-parse HEAD
# must print: 396db8a835d9787b2997de038b48f645c837e754
```

Then check the content of the `sd_card/` tree (37 files). The digests below were computed by Akam at
research time from a clone of that commit:

```bash
( cd toolkit/sd_card && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum )
# expected: 3aa3f698fd6d6f228ee785726970f708035799aa5bbcb73ef369bc3757747c2f  -
```

Key files (sha256, same origin):

```text
e2e8a0c226837ed71ba7489f78307c6ad37e0d9ce6385a8bb644365d65a92e62  hack.sh
bf976cab4282855b013ebe5c9908f593c1c651623eef35dcb6cabe64e7bce010  hostapd
020f022821eb560deb7df58cc62d4ee80e50d6db71c19bdbdd0499512a0543b1  custom/scripts/entrypoint.sh
83506e2b07b2395372790006b0aecb96788c1f984f2d352e676de9691e74f57a  custom/scripts/telnet.sh
85d704c9cbb7573725895063f7740023c22a53bc33fc3dbc4b4540c275f75dbf  custom/scripts/wifi_apply.sh
20eebe231a1f46a3d2af54fe44e1cdefe63bee9ce411dab1c8f0a96f058db778  custom/configs/hack.conf
```

Read `sd_card/hack.sh` and `sd_card/custom/scripts/*.sh` yourself before using them: they run as root
on your camera.

---

## 4. How the camera picks the card up (what runs)

**[Toolkit]** (README and scripts), summarised:

1. Boot chain: `init -> /etc/init.d/rcS -> rc.local -> /usr/sbin/main.sh -> /tmp/service.sh start`
   (`service.sh` is the copy of `/usr/sbin/service.sh`).
2. `service.sh` mounts the card on `/mnt` and tests marker files there. If **`_ht_ap_mode.conf`** exists,
   the firmware copies **`/mnt/hostapd`** to `/tmp/hostapd` and the Wi-Fi scripts execute it. The
   toolkit's `hostapd` is a short script that runs **`/mnt/hack.sh`**, as root, before `anyka_ipc` starts.
3. `hack.sh` mounts the card again on `/tmp/sd`, then (a) runs `dump_fs.sh` (skipped here, see section 5),
   (b) applies bind-mount overrides (RAM only, see section 10), (c) kills `anyka_ipc`, which is restarted
   through the toolkit's own `anyka_ipc.sh` wrapper that calls `custom/scripts/entrypoint.sh`.
4. `entrypoint.sh` starts, in the background: `telnet.sh`, `ftp.sh`, `offline.sh`, `wifi_apply.sh`, `onvif.sh`.
5. `telnet.sh`, when `TELNET=1`, runs **`telnetd -p ${TELNET_PORT} -l /bin/sh`** in a restart loop.
   **No login, no password.** Default port 24.
6. `wifi_apply.sh` with `MODE=sta` starts `wpa_supplicant` with a config in `/tmp` (RAM), calls the stock
   `/usr/sbin/station_connect.sh <security> <ssid> <pass>` and then `udhcpc` on `wlan0`.

Other markers in the tree: `_ak39_factory.ini` switches the stock firmware to its factory test mode
(telnet on port 23 with `root` / `telnet`, RTSP on 554; **[Toolkit]** README). The toolkit ships it with
an empty `[wifi_station]` section and so does the flasher; Akam leaves it as is, like the flasher.
Whether the port-23 service really appears on your unit is unverified; this guide uses port 24.

---

## 5. Prepare the card ([Flasher] stage 1, reproduced by hand)

Identify the card with care. **Formatting the wrong device destroys its data.**

```bash
lsblk -d -o NAME,SIZE,MODEL,TRAN,RM
DEV=/dev/sdX          # <-- the SD card, e.g. /dev/sdb. Double-check the size shown above.
```

If the desktop auto-mounts the card, unmount it (`sudo umount "$DEV"*`) or disable auto-mount first.
Then format as a single MBR + FAT32 partition (same as the flasher: `wipefs`, `sfdisk`, `mkfs.vfat`):

```bash
sudo wipefs -a "$DEV"
printf 'label: dos\n, , c, *\n' | sudo sfdisk "$DEV"
sudo partprobe "$DEV"; sleep 2
PART="${DEV}1"        # for /dev/mmcblkN use "${DEV}p1"
sudo mkfs.vfat -F 32 -n AKAMSD "$PART"
mkdir -p ~/akam-sdprep/mnt
sudo mount -o uid="$(id -u)",gid="$(id -g)",umask=022 "$PART" ~/akam-sdprep/mnt
```

`hack.sh` mounts `/dev/mmcblk0p1` (falling back to `/dev/mmcblk0`), so the card needs a partition
table with a first partition, as above. Keep `umask=022`: `hack.sh` tests `-x` on `dump_fs.sh`, and
the flasher uses the same mount options.

Copy the toolkit payload and apply the flasher's settings:

```bash
cd ~/akam-sdprep
M=mnt
cp -a toolkit/sd_card/. "$M"/
mkdir -p "$M/custom/state" "$M/logs"

# [Flasher] skip the toolkit's whole-filesystem copy (we do raw MTD dumps by hand instead)
: > "$M/custom/state/dump.done"
rm -f "$M/custom/state/shadow.done"

# [Flasher] telnet on 24, offline hosts file
sed -i -E 's/^TELNET=.*/TELNET=1/; s/^TELNET_PORT=.*/TELNET_PORT=24/; s/^OFFLINE=.*/OFFLINE=1/' \
    "$M/custom/configs/hack.conf"
grep -E '^(TELNET|TELNET_PORT|OFFLINE)=' "$M/custom/configs/hack.conf"   # expect 1 / 24 / 1

# [Flasher] Wi-Fi (the file is sourced by sh, so values are single-quoted)
q() { printf "'%s'" "${1//\'/\'\\\'\'}"; }
read -r  -p 'Wi-Fi SSID: ' SSID
read -rs -p 'Wi-Fi password: ' PASS; echo
printf 'MODE=sta\nSSID=%s\nPASS=%s\nSECURITY=3\nDHCP=1\n' "$(q "$SSID")" "$(q "$PASS")" \
    > "$M/custom/configs/wifi.conf"
unset PASS

# [Flasher] make entrypoint.sh idempotent and start it directly from hack.sh
sed -i '2i\# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0' \
    "$M/custom/scripts/entrypoint.sh"
cat >> "$M/hack.sh" <<'EOF_HOOK'
# DECLOUD_WIZARD_HOOK
/bin/sh "${SCRIPTS_DIR}/entrypoint.sh" >/dev/null 2>&1 &
EOF_HOOK

sync
```

Differences from the flasher's stage 1, on purpose: Akam does **not** copy the flasher's
`decloud_stage1.sh` / `decloud_flash.sh` to the card and does **not** add the hook line that runs
`decloud_stage1.sh` (that script also writes `/etc/config/wpa_supplicant.conf` into flash).
`SECURITY=3` means WPA/WPA2 (**[Toolkit]** `wifi.conf`: 1 open, 2 WEP, 3 WPA/WPA2). `ONVIF=1` and
the other toolkit defaults are left untouched, like the flasher (so ONVIF listens on port 8080 while the card is in).

```bash
cd ~ && sudo umount ~/akam-sdprep/mnt
```

---

## 6. Boot the camera from the card

1. Unplug the camera. Insert the card. Power it up (USB-C).
2. **[Flasher]** Wait two full minutes. The blue LED may keep blinking; the factory voice prompt is
   muted by the toolkit's `MUTE_FACTORY_PROMPT=1`, so you will probably not hear it.
3. Leave the camera powered while you work in the next sections.

If, after about three minutes, nothing answers (section 7), unplug, put the card back in the PC and
read `logs/*.log` on the card (`wifi.*.log`, `telnet.*.log`, `run.*.log`). The most common cause
reported by the flasher is a wrong SSID or password. Do not improvise further on the camera; the UART
route in [akam_v1_backup.md](akam_v1_backup.md) remains available.

---

## 7. Find the camera and connect

The camera gets an IP from DHCP. Look it up in your router's client list, or scan your LAN for the
port (replace the placeholder network with your own):

```bash
nmap -p 24 --open 192.0.2.0/24     # 192.0.2.0/24 is a placeholder
```

Connect (**[Toolkit]** port 24, no credentials, plain-text telnet):

```bash
telnet CAMERA_IP 24
```

You should get a `/bin/sh` prompt as root. If it asks for a login, you reached something else
(possibly the factory-mode telnet on port 23, `root` / `telnet` per the toolkit README). Stop and
reassess rather than continuing.

---

## 8. Dump the 8 partitions twice and compare (BusyBox shell on the camera)

Everything below runs in the telnet shell. `SD` is where the toolkit mounted the card.
**[Akam, untested]** unless noted. The `dd if=/dev/mtdblockN bs=64k` dump and the size check use the
same commands as **[Flasher]** stage 1.

### 8.1 Check card, free space and partition table

```sh
SD=/tmp/sd
mount | grep "$SD"            # must show the SD partition (mmcblk0p1), vfat, rw
df -k "$SD"                   # at least 17000 KiB free
cat /proc/version
cat /proc/mtd
```

Compare `/proc/mtd` with the table in [akam_v1_backup.md](akam_v1_backup.md) section 2 (the same
layout the flasher requires; these lines are what its `check_layout` greps for):

```sh
cat > /tmp/mtd.expected <<'EOF'
mtd0: 00032000 00001000 "UBOOT"
mtd1: 00001000 00001000 "ENV"
mtd2: 00001000 00001000 "ENVBK"
mtd3: 00010000 00001000 "DTB"
mtd4: 00180000 00001000 "KERNEL"
mtd5: 000fc000 00001000 "ROOTFS"
mtd6: 00040000 00001000 "CONFIG"
mtd7: 00500000 00001000 "APP"
EOF
grep -v '^dev:' /proc/mtd > /tmp/mtd.now
diff /tmp/mtd.now /tmp/mtd.expected && echo LAYOUT_OK
```

**If you do not see `LAYOUT_OK`, stop here.** Keep the card, do not flash anything.

### 8.2 Record the state of the camera (useful for the undo)

```sh
mount > "$SD/state_mounts.txt"
ls -la /etc/config > "$SD/state_etc_config.txt"
ps > "$SD/state_ps.txt"
```

### 8.3 Dump twice

```sh
for run in 1 2; do
    mkdir -p "$SD/dump$run"
    for i in 0 1 2 3 4 5 6 7; do
        dd if=/dev/mtdblock$i of="$SD/dump$run/mtdblock$i.bin" bs=64k
    done
    sync
done
```

### 8.4 Check sizes (exact bytes)

```sh
for run in 1 2; do
    i=0
    for want in 204800 4096 4096 65536 1572864 1032192 262144 5242880; do
        got=$(stat -c%s "$SD/dump$run/mtdblock$i.bin")
        if [ "$got" = "$want" ]; then echo "dump$run mtdblock$i OK $got"; else echo "dump$run mtdblock$i BAD $got (want $want)"; fi
        i=$((i+1))
    done
done
```

Any `BAD`: stop and do not use that dump.

### 8.5 Hash both dumps and compare them

`sha256sum` may be missing in the stock BusyBox; fall back to `md5sum` (the flasher uses `md5sum`):

```sh
if command -v sha256sum >/dev/null 2>&1; then H=sha256sum; else H=md5sum; fi
echo "using $H"
for run in 1 2; do (cd "$SD/dump$run" && $H mtdblock*.bin) > "$SD/sums_run$run.$H"; done
cat "$SD/sums_run1.$H"
diff "$SD/sums_run1.$H" "$SD/sums_run2.$H" && echo READS_IDENTICAL
```

Reading the result:

- `READS_IDENTICAL`: good.
- A difference in **`mtdblock6` (CONFIG) only**: possible and not necessarily a fault, because CONFIG is
  a JFFS2 filesystem mounted read/write on `/etc/config` while the camera runs, and the firmware may
  write to it between the two reads. Repeat both dumps. If it still differs only on `mtdblock6`, keep
  both versions and note it; do not guess which is right.
- A difference on any other partition: the read is unreliable. Do not use that dump; stop.

### 8.6 Finish

```sh
sync
ls -l "$SD"/dump1 "$SD"/dump2
```

Close the telnet session (`exit`), then **unplug the camera's power** (no halt command was verified on
this firmware) and only afterwards take out the card.

---

## 9. Copy to the PC and verify there

```bash
sudo mkdir -p /mnt/akamsd
sudo mount -o ro /dev/disk/by-label/AKAMSD /mnt/akamsd
mkdir -p ~/akam-backup
cp -a /mnt/akamsd/dump1 /mnt/akamsd/dump2 /mnt/akamsd/sums_run* /mnt/akamsd/state_*.txt /mnt/akamsd/logs ~/akam-backup/
sudo umount /mnt/akamsd
cd ~/akam-backup
```

Verify with the hash tool the camera used (`sha256sum` or `md5sum`):

```bash
(cd dump1 && sha256sum -c ../sums_run1.sha256sum)   # or: md5sum -c ../sums_run1.md5sum
(cd dump2 && sha256sum -c ../sums_run2.sha256sum)
for i in 0 1 2 3 4 5 6 7; do cmp -s dump1/mtdblock$i.bin dump2/mtdblock$i.bin || echo "DIFF mtdblock$i"; done
stat -c '%n %s' dump1/mtdblock*.bin
sha256sum dump1/mtdblock*.bin | tee SHA256SUMS.pc
file dump1/mtdblock7.bin; unsquashfs -s dump1/mtdblock7.bin
```

No output from the `cmp` loop means both dumps are byte-identical. The sizes must match
[akam_v1_backup.md](akam_v1_backup.md) section 2 exactly, and the checklist in its section 7
applies (two independent copies on different media, `SHA256SUMS.pc` stored separately). The build
guide expects the files as `mtdblockN.bin` in one directory; use `dump1/`.

---

## 10. What this method writes to the camera, and how to undo it

**Nothing is flashed.** No `mtd` device is opened for writing by this procedure. In the telnet session
only `dd if=/dev/mtdblockN` (read) is used.

What the toolkit's boot hook does on the camera, from reading `hack.sh` at the pinned commit
(**[Toolkit]**):

| Action | Where | Persistent? |
|---|---|---|
| `mount --bind` of the toolkit's `recover_cfg.sh` and `anyka_ipc.sh` over `/usr/sbin/...`, of `_ht_hw_settings.ini`, `_ht_sw_settings.ini`, `onvif_simple_server.conf`, `/etc/hosts` (offline mode), and of `shadow` over `/etc/config/shadow` | RAM (mount table) | No: gone at reboot without the card |
| `wpa_supplicant` config, `udhcpc`, `telnetd`, ONVIF processes | `/tmp`, RAM | No |
| Logs and state files | the SD card | Only on the card |
| `touch /etc/config/shadow` and `touch /etc/config/log_debug` (done before the bind mounts) | **CONFIG partition (JFFS2, flash)** | **Possibly yes.** If these files do not already exist, an empty file is created. Unverified whether they exist on stock; `state_etc_config.txt` (step 8.2) was taken after the hook ran, so it cannot tell you. |

Consequences:

- **Your CONFIG dump (`mtdblock6.bin`) is taken after the toolkit hook ran**, so it may include those
  empty files and updated timestamps. Akam expects it to be functionally the stock CONFIG; this is
  not proven. The other seven partitions are not written by the hook. The flasher takes its
  "pristine" dump at the same point (hook first, then dump), so it has the same property.
- Unverified: whether the stock `station_connect.sh` (called by the toolkit with your SSID and
  password) saves the network into CONFIG. The flasher's stage 1 comments say the script only adds the
  network dynamically and that the flasher copies it to CONFIG itself. Akam did not read
  `station_connect.sh` (it lives in the camera's APP partition; read it in your dump afterwards if you
  care).
- While the card is in, the toolkit's `recover_cfg.sh` replaces the stock one and the camera's
  settings files are served from the card. This also ends when the card is removed.

**Undo:** power off, remove the card, power on. The camera then boots its stock firmware from flash;
the RAM-only changes vanish. Nothing else has to be restored by this method, apart from the possible
empty files above (harmless, not verified). Then reformat the card (section 11).

---

## 11. Security notes

- **Do not run stage 2 and do not flash anything.** This guide ends once the backup is verified.
  Flashing is a separate step with its own guide ([flash and restore](akam_v1_flash_and_restore.md)).
  Do not run the flasher's `install.sh` (section 1) and do not put `decloud_flash.sh` on the card.
- **Telnet on port 24 has no password and is unencrypted.** Anyone on your LAN or Wi-Fi can get a root
  shell on the camera for as long as the card is inserted and the camera is powered. Use a trusted
  network, do the backup in one session, then power off and **remove the card**. Without the card the
  toolkit's telnet is not started.
- The same applies to the other services the toolkit starts (ONVIF on port 8080).
- The card holds your **Wi-Fi password in clear text** (`custom/configs/wifi.conf`) and, after the
  dump, the full firmware including `mtdblock6.bin` (Wi-Fi credentials, device identifiers). **Never
  publish, commit, attach or share any of it** (`dump1/`, `dump2/`, `logs/`, `state_*.txt`).
- After copying, reformat the card (`sudo mkfs.vfat -F 32 /dev/...` after `wipefs -a`). On flash media,
  deleting files does not reliably erase data; if the card is ever lost or given away, change the Wi-Fi password.
