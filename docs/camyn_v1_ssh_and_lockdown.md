# Camyn V1: SSH Provisioning and Telnet/FTP Lockdown

Camyn replaces the stock remote access (Telnet 23, FTP 21, and whatever the stock boot scripts
open) with one service: **SSH on port 22, root login by public key only**, no password. The SSH
server is [Dropbear](https://github.com/mkj/dropbear) 2026.94 built without password authentication,
forwarding, RSA, ECDSA or DSS.

Status: the scripts described here are tested only on the build host, against fake binaries and a
synthetic fixture (`tests/ssh_init_test.sh`, `tests/lockdown_test.sh`). **Nothing has run on a camera.**
Items marked *(assumed)* are hypotheses listed in the [test plan](camyn_v1_test_plan.md#3-hypotheses-to-verify).

---

## 1. Generate a dedicated key

Use a key made only for this camera (one key per camera; never reuse your everyday key):

```bash
ssh-keygen -t ed25519 -f ~/.ssh/camyn -C "camyn"
```

Set a passphrase. This creates `~/.ssh/camyn` (private, stays on your PC, never copied anywhere) and
`~/.ssh/camyn.pub` (public). Only **ed25519** keys are accepted by Camyn V1.

## 2. Put the public key on the microSD card

Camyn embeds **no key** in the image. At boot, `camyn_ssh.sh` reads the public keys from the card and
copies them into the persistent CONFIG partition.

Exact location: a directory `camyn` at the root of the FAT32 card, containing the file
`authorized_keys`. On the real firmware the stock `service.sh` mounts the card on `/mnt` only for its
own boot checks and then runs `umount -l /mnt`, so `/mnt/camyn` is normally not visible afterwards.
`camyn_ssh.sh` therefore mounts the card read-only on `/tmp/camyn_sd` (same devices and filesystem
order as the stock `tf_mount.sh`: vfat then exfat, `/dev/mmcblk0p1` then `/dev/mmcblk0`), reads
`camyn/authorized_keys` and unmounts. *(Not yet tested on hardware.)*

```bash
# the card is mounted on your PC at /run/media/$USER/CARD (adapt)
mkdir -p /run/media/$USER/CARD/camyn
cp ~/.ssh/camyn.pub /run/media/$USER/CARD/camyn/authorized_keys
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

Insert the card **before applying power**. The keys are read only when `camyn_ssh.sh` starts at boot, so
a card inserted later has no effect until the next power cycle.

At boot the wrapper (`ak_rtsp_wrapper.sh`) starts `camyn_ssh.sh` in the background before it brings up
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

It writes one line per step to `/tmp/camyn_ssh.log` (RAM; no secret is logged). If `/etc/config` never
becomes writable it still starts Dropbear with an ephemeral host key in `/tmp` and **no** persistent
keys, and logs that.

The host key and `authorized_keys` live in CONFIG, so they survive power cuts and reboots *(to be
confirmed by the persistence test)*. After the first successful login you can remove the card.

Connect:

```bash
ssh -i ~/.ssh/camyn -o IdentitiesOnly=yes root@<camera-ip>
```

Find `<camera-ip>` in your router's DHCP list. On the first connection, compare the displayed
ed25519 fingerprint with the one the camera computes (from the UART console):
`/usr/bin/dropbearkey -y -f /etc/config/dropbear/dropbear_ed25519_host_key`.

### Changing keys later

Put a new `camyn/authorized_keys` on the card and power cycle. A valid file **replaces** the whole
stored list.

### Host key change after a CONFIG wipe

The host key is stored in CONFIG. If CONFIG is erased (factory reset, restoring `mtdblock6`, a full
restore), a new host key is generated at the next boot and `ssh` will warn that the remote host
identification changed. Verify the new fingerprint over UART as above, then:

```bash
ssh-keygen -R <camera-ip>
```

A CONFIG wipe also deletes `authorized_keys`. With no embedded default key, you must insert the card
with `camyn/authorized_keys` again, or fall back to the UART console. Wi-Fi settings live in the same
partition (`/etc/config/wpa_supplicant.conf`), so a wipe also drops the camera off your network.

### `/etc/shells`

Dropbear refuses a login if `/etc/shells` exists and does not list the user's shell. If the file exists
without `/bin/sh`, `camyn_ssh.sh` bind-mounts a private one-line list over it (the real file is not
modified) and logs it. If that mount fails the log says `root login may be refused`.
`camyn_hwcheck.sh` checks that root's shell is listed. Whether stock has `/etc/shells` is
unknown *(assumed absent or containing `/bin/sh`)*.

### If you cannot log in

From the UART console: `cat /tmp/camyn_ssh.log`, `/usr/sbin/camyn_ssh.sh status`, and check that
`/etc/config/dropbear` is mode 700 and `authorized_keys` mode 600 owned by root. Dropbear refuses
`authorized_keys` when any directory on the path is writable by group or others; the script logs the
modes of `/etc/config`, `/etc` and `/` for this reason.

## 4. Network exposure of SSH

Dropbear listens on all interfaces, port 22, public-key only (no password prompt exists in the
binary). There is no rate limit beyond `-T 3` (three authentication attempts per connection). Keep
the camera on a trusted segment (see [network hardening](camyn_v1_install.md#5-network-hardening)).

## 5. File transfer without `scp`

`scp` and SFTP are **intentionally absent**: the image ships neither an `scp` binary nor an SFTP
server, and OpenSSH 9 and later `scp` speaks SFTP by default. Use `ssh` with `cat`:

```bash
# PC -> camera (RAM-backed /tmp; the camera has 64 MiB of RAM, keep files small)
ssh -i ~/.ssh/camyn root@<camera-ip> 'cat > /tmp/f && chmod +x /tmp/f' < f

# camera -> PC
ssh -i ~/.ssh/camyn root@<camera-ip> 'cat /tmp/f' > f

# a directory, PC -> camera (needs gzip support in the camera's BusyBox tar: a hypothesis to verify)
tar czf - dir | ssh -i ~/.ssh/camyn root@<camera-ip> 'tar xzf - -C /tmp'
```

`make -C src/ak_rtsp upload CAMERA_IP=<camera-ip>` uses the `cat` technique, with plain
`ssh root@<camera-ip>`; add a host entry so it picks your key:

```
# ~/.ssh/config
Host <camera-ip>
    IdentityFile ~/.ssh/camyn
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
  includes `tcpsvd ... ftpd`) is replaced by `: # CAMYN-DISABLED: <original line>`, so the daemons are
  never started;
- lines that run the microSD hook (`_ht_ap_mode.conf`, `hack.sh`, `hostapd` on the card, `telnet.sh`) get
  a guard so that they run **only if the maintenance flag exists** (section 7);
- no other line changes; the exact diff is written to `service_sh.diff` next to the image.

The build fails (exit 2 or 3) if the file has a shape the patch will not guess at, or no launch line is
found, unless you set `CAMYN_ALLOW_UNPATCHED_SERVICE=1`.

On the real stock firmware (audited from a dump) the `service.sh` and `main.sh` launch lines for
`telnetd` and `tcpsvd ... ftpd` are already commented out, and nothing in the stock tree runs
`hack.sh`. The only SD-controlled code path in `service.sh` is the AP-mode marker: if `_ht_ap_mode.conf`
is on the card, the card's `hostapd` is copied to `/tmp` and later run as root by `wifi_driver.sh`. The
patch accepts "commented-only" daemons (exit 0 with a NOTE) as long as it finds this hook, and guards
the `dd` of the marker file and the `cp` of the card's `hostapd` with the maintenance flag. The stock
`tf_update.sh` (a `*.bin` on the card triggers `system_upgrade`) is a separate SD-triggered path that
Camyn does not change.

### Layer 2: runtime safety net, `camyn_lockdown.sh`

Started in the background by the wrapper next to `camyn_ssh.sh`. It covers a layer-1 patch that had no
effect, or a late start by another script. It:

- kills processes whose command line has a token named `telnetd`, `utelnetd`, `ftpd`, `bftpd`,
  `vsftpd`, or one of the SD-hook respawn scripts `telnet.sh` and `hack.sh` (respawn scripts first);
- runs every 5 s for 2 minutes after boot, then every 60 s without end, so a restart after a Wi-Fi
  reconnect is also caught;
- never touches `ak_rtsp`, `dropbear`, `wpa_supplicant`, `cmdserver`, `msgserver` or `camyn_*`;
- uses only `ps` and `kill` (no `pkill`/`killall`), and opens no network socket;
- logs to `/tmp/camyn_lockdown.log`.

The stock `anyka_ipc` is removed from the image, which also removes its factory-test mode that opens
Telnet on port 23.

### Exposure window

| Situation | Window in which a telnet/FTP daemon can be reachable |
|---|---|
| Daemons started by APP `service.sh` and layer 1 applied | None: they are never launched |
| Layer 1 had no effect, only layer 2 acts | From the stock launch until the first pass of `camyn_lockdown.sh`; on the order of tens of seconds *(estimate, not measured)* |
| Daemons launched from ROOTFS (`rcS`, `inittab`, `rc.local`) | Until the first pass of the safety net after the wrapper starts; ROOTFS is not modified in V1, so this cannot be fixed without a decision to modify ROOTFS |

Measure it on your unit with `camyn_hwcheck.sh --boot-window` ([test plan](camyn_v1_test_plan.md#2-post-flash-procedure)).

## 7. Maintenance flag

Create an **empty file named `CAMYN_MAINTENANCE` at the root of the microSD card**. The camera is
expected to read it as `/mnt/CAMYN_MAINTENANCE` *(the patched `service.sh` sees it there during early
boot, while the card is mounted; later the stock script unmounts `/mnt`, so `camyn_lockdown.sh` will
normally not see the flag and keeps killing. With the real firmware nothing starts telnet or FTP anyway,
so this only matters for the AP-mode hook)*.

- With the flag present: the patched `service.sh` lets the card's hook scripts run again, and
  `camyn_lockdown.sh` logs `maintenance mode`, kills nothing and keeps looping; as soon as the flag disappears it resumes killing at the next round. The flag does **not** start Telnet or
  FTP by itself: it only stops Camyn from preventing whatever the card's scripts start.
- Without the flag: everything is closed (including Telnet/FTP left running after the card was removed).
- Every 60 s the safety net also runs `camyn_ssh.sh start` in the background, which restarts a dead Dropbear.
- The flag is checked at boot and re-checked every round of the safety net. It is never set by the
  watchdog or by a network event; only physical access to the card can enable it. Treat it as exactly
  that: whoever can put a card in the camera can run code as root, as with the stock firmware.

If you ever prepared a card with the Action-Smart-Connect-Flasher or the `lsc-tuya-toolkit`, its hook
(`hack.sh`, `telnet.sh`, which opens an unauthenticated telnet on port 24) and its `wifi.conf` stay on
that card. Without the flag the patched `service.sh` does not run them, but **empty the card before
normal use anyway**.

## 8. Test order: keep a way back until SSH is proven

1. Wire UART first and keep it attached for the whole first test ([hardware_and_uart.md](hardware_and_uart.md)).
   After the flash there is no network shell except SSH; UART is the fallback.
2. Prepare the card with `camyn/authorized_keys` **and** an empty `CAMYN_MAINTENANCE`.
3. Flash, power cycle with the card inserted, and wait a few minutes for the first boot.
4. Log in with `ssh -i ~/.ssh/camyn ...`. Run `camyn_hwcheck.sh` (it will WARN about the flag).
5. Power-cut the camera, boot again, and log in again with the same key and the same host fingerprint.
6. Only then remove `CAMYN_MAINTENANCE`, power cycle, and run `camyn_hwcheck.sh` and
   `camyn_hwcheck.sh --boot-window` again; ports 21, 23 and 24 must be closed. Then remove the card
   and repeat the login (boot without a card).

Details of each check: [test plan](camyn_v1_test_plan.md).
