# Akam V1: SSH Provisioning and Telnet/FTP Lockdown

Akam replaces the stock remote access (Telnet 23, FTP 21, and whatever the stock boot scripts
open) with one service: **SSH on port 22, root login by public key only**, no password. The SSH
server is [Dropbear](https://github.com/mkj/dropbear) 2026.94 built without password authentication,
forwarding, RSA, ECDSA or DSS.

> **Reference page.** The wizard does the key and card work for you (`./akam prepare-flash-sd`,
> `./akam flash`, `./akam verify`); this page explains what the camera-side scripts do and how to use the
> manual route.

Status: SSH start, key login, persistence across power cuts and the Telnet/FTP lockdown were validated on
two cameras (2026-11-07 to 11-09) and are covered on the build host by `tests/ssh_init_test.sh` and
`tests/lockdown_test.sh`. Items marked *(assumed)* are hypotheses listed in the
[test plan](akam_v1_test_plan.md#3-hypotheses-to-verify); one check remains: Telnet staying off after a
Wi-Fi reconnect.

---

## 1. Generate a dedicated key

Use a key made only for this camera (one key per camera; never reuse your everyday key):

```bash
ssh-keygen -t ed25519 -f ~/.ssh/akam -C "akam"
```

Set a passphrase. (The wizard offers your existing ed25519 keys to choose from if `~/.ssh/akam.pub` is
missing, or creates this dedicated one.) This creates `~/.ssh/akam` (private, stays on your PC, never copied anywhere) and
`~/.ssh/akam.pub` (public). Only **ed25519** keys are accepted by Akam V1.

## 2. Put the public key on the microSD card

Akam embeds **no key** in the image. At boot, `akam_ssh.sh` reads the public keys from the card and
copies them into the persistent CONFIG partition.

Exact location: a directory `akam` at the root of the FAT32 card, containing the file
`authorized_keys`. `akam_ssh.sh` first looks for the card where it is **already mounted**: it parses
`/proc/mounts` for `/dev/mmcblk0p1` (or `/dev/mmcblk0`) mounted on a directory, for instance `/tmp/sd`
when the flasher toolkit mounted it (its file bind-mounts are ignored) or `/mnt`, and reads
`<mountpoint>/akam/authorized_keys`. Only if the card is not mounted anywhere does it mount it
read-only on `/tmp/akam_sd` (the stock `service.sh` unmounts `/mnt` after its boot checks; same devices
and filesystem order as the stock `tf_mount.sh`: vfat then exfat, `/dev/mmcblk0p1` then `/dev/mmcblk0`),
then unmounts. Every step, including mount errors, is logged in `/tmp/akam_ssh.log`.

> **Deprecated alias (one release).** A card prepared for the early-development image (formerly Camyn)
> has the folder `camyn/` instead. `akam_ssh.sh` still reads `camyn/authorized_keys` when
> `akam/authorized_keys` is absent and logs a `DEPRECATED` line in `/tmp/akam_ssh.log`. Rename the folder
> to `akam` on your card; the fallback is removed in the next release.

```bash
# the card is mounted on your PC at /run/media/$USER/CARD (adapt)
mkdir -p /run/media/$USER/CARD/akam
cp ~/.ssh/akam.pub /run/media/$USER/CARD/akam/authorized_keys
sync
```

Rules enforced by the script:

- Only plain `ssh-ed25519 <base64> [comment]` lines are accepted. Empty lines and `#` comments are skipped.
- Any line with `command=` is rejected, and so is any line with options or another key type (RSA, ECDSA).
- If no valid line remains, the existing CONFIG copy is left untouched.
- The file on the card is never modified or deleted.
- **Never put a private key on the card.** The image builder also refuses private-key material, but the
  card is outside its reach.

## 3. First boot

Insert the card **before applying power**. The keys are read only when `akam_ssh.sh` starts at boot, so
a card inserted later has no effect until the next power cycle.

At boot the wrapper (`ak_rtsp_wrapper.sh`) starts `akam_ssh.sh` in the background before it brings up
Wi-Fi and starts `ak_rtsp`; the helper can never delay or stop `ak_rtsp` (its exit status is ignored).
It then:

1. takes a lock so a respawned wrapper cannot start a second copy;
2. waits up to about 30 s for `/etc/config` (CONFIG, JFFS2) to be writable;
3. creates `/etc/config/dropbear` (mode 0700);
4. generates the ed25519 host key `dropbear_ed25519_host_key` on the **first** boot (it may wait for
   entropy, so the first SSH connection can come late *(assumed)*);
5. validates the card's keys and installs them atomically as `/etc/config/dropbear/authorized_keys`
   (0600, root) when they differ from the stored copy;
6. makes sure a login shell is accepted (section 4);
7. starts `dropbear` on port 22 with `-r <hostkey> -D /etc/config/dropbear -P /tmp/dropbear.pid -T 3 -E`.

It writes one line per step to `/tmp/akam_ssh.log` (RAM; no secret is logged). If `/etc/config` never
becomes writable it still starts Dropbear with an ephemeral host key in `/tmp` and **no** persistent
keys, and logs that.

The host key and `authorized_keys` live in CONFIG, so they survive power cuts and reboots (confirmed on hardware). After the first successful login you can remove the card.

Connect:

```bash
ssh -i ~/.ssh/akam -o IdentitiesOnly=yes root@<camera-ip>
```

Find `<camera-ip>` in your router's DHCP list. On the first connection, compare the displayed
ed25519 fingerprint with the one the camera computes (from the UART console):
`/usr/bin/dropbearkey -y -f /etc/config/dropbear/dropbear_ed25519_host_key`.

### Changing keys later

Put a new `akam/authorized_keys` on the card and power cycle. A valid file **replaces** the whole
stored list.

### Host key change after a CONFIG wipe

The host key is stored in CONFIG. If CONFIG is erased (factory reset, restoring `mtdblock6`, a full
restore), a new host key is generated at the next boot and `ssh` will warn that the remote host
identification changed. Verify the new fingerprint over UART as above, then:

```bash
ssh-keygen -R <camera-ip>
```

A CONFIG wipe also deletes `authorized_keys`. With no embedded default key, you must insert the card
with `akam/authorized_keys` again, or fall back to the UART console. Wi-Fi settings live in the same
partition (`/etc/config/wpa_supplicant.conf`), so a wipe also drops the camera off your network.

### `/etc/shells`

Dropbear refuses a login if `/etc/shells` exists and does not list the user's shell. If the file exists
without `/bin/sh`, `akam_ssh.sh` bind-mounts a private one-line list over it (the real file is not
modified) and logs it. If that mount fails the log says `root login may be refused`.
`akam_hwcheck.sh` checks that root's shell is listed. Whether stock has `/etc/shells` is
unknown *(assumed absent or containing `/bin/sh`)*.

### If you cannot log in

From the UART console: `cat /tmp/akam_ssh.log`, `/usr/sbin/akam_ssh.sh status`, and check that
`/etc/config/dropbear` is mode 700 and `authorized_keys` mode 600 owned by root. Dropbear refuses
`authorized_keys` when any directory on the path is writable by group or others; the script logs the
modes of `/etc/config`, `/etc` and `/` for this reason.

The stock ROOTFS is a read-only squashfs whose `/` and `/etc` are mode 0775 (group-writable), which
Dropbear would reject, and they cannot be changed at runtime. The walk stops at root's HOME, so
`akam_ssh.sh` makes HOME a private tmpfs directory (`/tmp/akam_home`, 0700) holding a copy of the key
file, and bind-mounts a private copy of `passwd` over `/etc/passwd` (it is a symlink to
`/etc/config/passwd`) whose `root` entry has that HOME (a missing `root` entry is added: without one
Dropbear refuses every login). The real file is untouched. The log line is `root HOME is now ...`; if
it says `could not set root HOME` the legacy directory is used and the login may be refused.
The bind mount is made once per boot (a respawned script sees the root entry already points at
`/tmp/akam_home` and does not mount again). If `/etc/config/passwd` does not exist, the script creates
it **empty** in CONFIG (a persistent write) so that there is a file to bind over; with the stock file
present nothing in CONFIG is written. Root's HOME (and so the shell's starting directory) is on tmpfs
and is empty after every reboot.

## 4. Network exposure of SSH

Dropbear listens on all interfaces, port 22, public-key only (no password prompt exists in the
binary). There is no rate limit beyond `-T 3` (three authentication attempts per connection). Keep
the camera on a trusted segment (see [network hardening](akam_v1_install.md#5-network-hardening)).

## 5. File transfer without `scp`

`scp` and SFTP are **intentionally absent**: the image ships neither an `scp` binary nor an SFTP
server, and OpenSSH 9 and later `scp` speaks SFTP by default. Use `ssh` with `cat`:

```bash
# PC -> camera (RAM-backed /tmp; the camera has 64 MiB of RAM, keep files small)
ssh -i ~/.ssh/akam root@<camera-ip> 'cat > /tmp/f && chmod +x /tmp/f' < f

# camera -> PC
ssh -i ~/.ssh/akam root@<camera-ip> 'cat /tmp/f' > f

# a directory, PC -> camera (needs gzip support in the camera's BusyBox tar: a hypothesis to verify)
tar czf - dir | ssh -i ~/.ssh/akam root@<camera-ip> 'tar xzf - -C /tmp'
```

`make -C src/ak_rtsp upload CAMERA_IP=<camera-ip>` uses the `cat` technique, with plain
`ssh root@<camera-ip>`; add a host entry so it picks your key:

```
# ~/.ssh/config
Host <camera-ip>
    IdentityFile ~/.ssh/akam
    IdentitiesOnly yes
```

---

## 6. Telnet and FTP lockdown

Two independent layers; both are needed because the exact place where the stock firmware starts
`telnetd` / `ftpd` on your unit is not known until your dump has been read
*(assumed: `sbin/service.sh` in the APP)*.

### Layer 1: build-time patch of `sbin/service.sh`

`tools/firmware_patch_templates/patch_service_sh.sh`, run by `build_firmware.sh`:

- every non-comment line that launches `telnetd`, `utelnetd`, `ftpd`, `bftpd` or `vsftpd` (this
  includes `tcpsvd ... ftpd`) is replaced by `: # AKAM-DISABLED: <original line>`, so the daemons are
  never started;
- lines that run the microSD hook (`_ht_ap_mode.conf`, `hack.sh`, `hostapd` on the card, `telnet.sh`) get
  a guard so that they run **only if the maintenance flag exists** (section 7);
- no other line changes; the exact diff is written to `service_sh.diff` next to the image.

The build fails (exit 2 or 3) if the file has a shape the patch will not guess at, or no launch line is
found, unless you set `AKAM_ALLOW_UNPATCHED_SERVICE=1`.

On the real stock firmware (audited from a dump) the `service.sh` and `main.sh` launch lines for
`telnetd` and `tcpsvd ... ftpd` are already commented out, and nothing in the stock tree runs
`hack.sh`. The only SD-controlled code path in `service.sh` is the AP-mode marker: if `_ht_ap_mode.conf`
is on the card, the card's `hostapd` is copied to `/tmp` and later run as root by `wifi_driver.sh`. The
patch accepts "commented-only" daemons (exit 0 with a NOTE) as long as it finds this hook, and guards
the `dd` of the marker file and the `cp` of the card's `hostapd` with the maintenance flag. The stock
`tf_update.sh` (a `*.bin` on the card triggers `system_upgrade`) is a separate SD-triggered path that
Akam does not change.

### Layer 2: runtime safety net, `akam_lockdown.sh`

Started in the background by the wrapper next to `akam_ssh.sh`. It covers a layer-1 patch that had no
effect, or a late start by another script. It:

- kills processes whose command line has a token named `telnetd`, `utelnetd`, `ftpd`, `bftpd`,
  `vsftpd`, or one of the SD-hook respawn scripts `telnet.sh` and `hack.sh` (respawn scripts first);
- runs every 5 s for 2 minutes after boot, then every 60 s without end, so a restart after a Wi-Fi
  reconnect is also caught;
- never touches `ak_rtsp`, `dropbear`, `wpa_supplicant`, `cmdserver`, `msgserver` or `akam_*`;
- uses only `ps` and `kill` (no `pkill`/`killall`), and opens no network socket;
- logs to `/tmp/akam_lockdown.log`.

The stock `anyka_ipc` is removed from the image, which also removes its factory-test mode that opens
Telnet on port 23.

### Exposure window

| Situation | Window in which a telnet/FTP daemon can be reachable |
|---|---|
| Daemons started by APP `service.sh` and layer 1 applied | None: they are never launched |
| Layer 1 had no effect, only layer 2 acts | From the stock launch until the first pass of `akam_lockdown.sh`; on the order of tens of seconds *(estimate, not measured)* |
| Daemons launched from ROOTFS (`rcS`, `inittab`, `rc.local`) | Until the first pass of the safety net after the wrapper starts; ROOTFS is not modified in V1, so this cannot be fixed without a decision to modify ROOTFS |

Measure it on your unit with `akam_hwcheck.sh --boot-window` ([test plan](akam_v1_test_plan.md#2-post-flash-procedure)).

## 7. Maintenance flag

Create an **empty file named `AKAM_MAINTENANCE` at the root of the microSD card**. The stock
`service.sh` mounts the card on `/mnt`, runs the SD hooks and then unmounts it (`umount -l /mnt`)
long before `akam_lockdown.sh` starts. So the patched `service.sh` records the flag in RAM while
the card is still mounted: right before the first guarded SD hook it runs
`{ [ -f /mnt/AKAM_MAINTENANCE ] || [ -f /mnt/CAMYN_MAINTENANCE ]; } && : > /tmp/akam_maintenance`
(the second test is the deprecated alias, see below). `akam_lockdown.sh` and
`akam_hwcheck.sh` honour that RAM marker (and still the `/mnt/...` file while a card is mounted).

- With the flag present at boot: the patched `service.sh` lets the card's hook scripts run again, and
  `akam_lockdown.sh` logs `maintenance mode`, kills nothing and keeps looping. The flag does **not**
  start Telnet or FTP by itself: it only stops Akam from preventing whatever the card's scripts start.
- **Duration:** maintenance mode lasts **until the next reboot without the flag**. The marker lives in
  RAM (`/tmp`), so removing the card does not end it; a power cycle with the card absent (or without
  the flag file) does. Deleting `/tmp/akam_maintenance` by hand over a shell also resumes the
  lockdown at the next round.
- Without the flag: everything is closed (including Telnet/FTP left running after the card was removed).
- Every 60 s the safety net also runs `akam_ssh.sh start` in the background, which restarts a dead Dropbear.
- The flag is read once at boot (into the RAM marker); the marker is re-checked every round of the
  safety net. It is never set by the
  watchdog or by a network event; only physical access to the card can enable it. Treat it as exactly
  that: whoever can put a card in the camera can run code as root, as with the stock firmware.

**Deprecated alias (one release).** The flag was named `CAMYN_MAINTENANCE` in the early-development image
(formerly Camyn). A card that still carries that file keeps working: the patched `service.sh`, the
safety net and `akam_hwcheck.sh` accept it as an alias of `AKAM_MAINTENANCE`, and the last two log or
print a `DEPRECATED` note. Rename the file on your card. The alias is removed in the next release; to
build an image without it, set `AKAM_MAINT_FLAG_LEGACY=` (empty) when running `patch_service_sh.sh`.

If you ever prepared a card with the Action-Smart-Connect-Flasher or the `lsc-tuya-toolkit`, its hook
(`hack.sh`, `telnet.sh`, which opens an unauthenticated telnet on port 24) and its `wifi.conf` stay on
that card. Without the flag the patched `service.sh` does not run them, but **empty the card before
normal use anyway**.

## 8. Test order: keep a way back until SSH is proven

The manual order (advanced). `./akam flash` then `./akam verify` follow the same sequence: card with the key,
flash, SSH and checks, then a boot without the card.

1. Wire UART first and keep it attached for the whole first test ([hardware_and_uart.md](hardware_and_uart.md)).
   After the flash there is no network shell except SSH; UART is the fallback. Without UART, read
   [what can and cannot be recovered](akam_v1_flash_and_restore.md#without-uart-what-can-be-recovered)
   and skip the flag in step 2.
2. Prepare the card with `akam/authorized_keys`. **First-boot card (recommended, also without UART):**
   `tools/sd/prepare_first_boot_sd.sh` builds a card with the pinned toolkit payload (telnet 24) plus
   `AKAM_MAINTENANCE` plus `akam/authorized_keys`: the first boot then offers telnet 24 (kept alive by
   the RAM marker) and SSH 22. After SSH is validated, remove the card and reboot; telnet must be gone
   ([flash guide](akam_v1_flash_and_restore.md#first-flash-without-uart-from-the-stock-firmware), step 5).
   Do not put the flag on any other card that holds toolkit/flasher files.
3. Flash, power cycle with the card inserted, and wait a few minutes for the first boot.
4. Log in with `ssh -i ~/.ssh/akam ...`. Run `akam_hwcheck.sh` (it will WARN about the flag).
5. Power-cut the camera, boot again, and log in again with the same key and the same host fingerprint.
6. Only then remove `AKAM_MAINTENANCE`, power cycle, and run `akam_hwcheck.sh` and
   `akam_hwcheck.sh --boot-window` again; ports 21, 23 and 24 must be closed. Then remove the card
   and repeat the login (boot without a card).

Details of each check: [test plan](akam_v1_test_plan.md).
