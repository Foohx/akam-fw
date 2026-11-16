# LSC Smart Rotatable Camera 2K (Dual Band) -- `lsc-rotatable-2k-ak3918av130`

Firmware kind: **`decloud-overlay`**. Unlike the 1080p ([`src/ak_rtsp`](../../src/ak_rtsp), a native app that
replaces the APP partition), this camera keeps its stock app (`yy_cam`, YYOS/TuyaOS) and Akam adds an SD card that
boots it fully local: no cloud, local RTSP/ONVIF, key-only SSH, a telnet rescue. **Status: untested** (no hardware
validation yet). Plan: [docs/porting/lsc-rotatable-2k/PLAN.md](../../docs/porting/lsc-rotatable-2k/PLAN.md); model
index: [docs/models.md](../../docs/models.md); first-boot checklist:
[phase1-validation.md](../../docs/porting/lsc-rotatable-2k/phase1-validation.md).

Camera: Action article `3225722`, label `SZ G26312`; SoC Anyka AK3918AV130, kernel 4.4.302-cip94, 8 MiB SPI NOR, 6 MTD
partitions. Everything model-specific lives in this directory.

## Layout

| Path | What |
|---|---|
| `overlay/` | the Akam card: `yy_debug.ini` (`mode=debug`, `start_sh=/mnt/sdcard/akam_boot.sh`), `akam_boot.sh`, `akam/*.sh` |
| `rescue/` | the minimal telnet-only rescue card: `yy_debug.ini`, `hack.sh`, `telnetd` |
| `tools/` | host-side helpers (empty for now) |
| `ptz/` | `akam_ptz`, the tiny on-camera motor tool (C, static ARM; `ptz/build_akam_ptz.sh`; copied to `akam/akam_ptz` by `build_card.sh`; soft-limited motion layer in `ptz/akam_ptz_safe.h`, optional `akam/ptz.conf`; init, moves and the safe layer hardware verified; the same layer drives the motor inside `akam2k_rtsp` through `app/hal_ptz.*`, see ptz-findings.md "hal_ptz") |
| `overlay/akam/own_app_run.sh` | supervisor of `akam2k_rtsp` (see own-app-feasibility.md, "Auto-start"); the app sources and `make poc3-dist` are in `app/` |
| `sntp/` + `overlay` stub | opt-in time sync: `akam2k_sntp` (one-shot SNTP client, BusyBox has no `ntpd`) driven by the shared `akam_ntp.sh`; enabled only by creating `akam/ntp` on the card (`server=NAME` lines, e.g. your router's IP), see own-app-feasibility.md, "Time" |
| `build_card.sh` | assembles `overlay/` or `rescue/` into a directory (the mounted card) |

## Build a card

You mount the card yourself (any FAT32 card, label up to you). The script only copies files into the directory you
give it: it never formats, partitions, mounts, or writes a device, and refuses `/` or a non-directory.

```sh
# Overlay card (needs your SSH public keys, ed25519 only; they are never taken from the repo):
tools/build/bootstrap.sh && tools/dropbear/build_dropbear.sh     # once: the shimmed static Dropbear
firmware/lsc-rotatable-2k-ak3918av130/build_card.sh /path/to/mounted/card --authorized-keys ~/.ssh/id_ed25519.pub --decloud

# Rescue card (telnet root/root only):
firmware/lsc-rotatable-2k-ak3918av130/build_card.sh /path/to/mounted/card --rescue-only
```

Options: `--decloud` (block WAN at boot), `--enable-param-conf` (opt-in edit of `/config/param.conf`, see below),
`--no-telnet` (overlay without the passwordless telnet rescue). Flags are empty files in `akam/` on the card; you can
add or remove them by hand.

The static busybox (`busybox-1.36.1-ak3918-armv5te-static`) is **not committed**: the script downloads it from the
GitHub release `busybox-v1.36.1` of `alex-so-3/lsc-ak3918-yyos-root` and verifies its sha256
(`367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290`); a mismatch is a hard failure and nothing is
written. Offline: `AKAM_ROT2K_BUSYBOX=/path/to/file` (still verified). Dropbear comes from
`tools/dropbear/build_dropbear.sh`; `build_card.sh` refuses a build that is not the **time32-shimmed** one of the
current `src/common/time32_shim.c` (kernel 4.4 does not run musl's time64 `clock_gettime` syscall, the same bug as
the 1080p).

## What the overlay does at boot

`akam_boot.sh` (run by the stock `init.sh` instead of `start.sh`): (1) telnet rescue + tmpfs shadow `root/root`;
(2) Dropbear key-only root on :22 (keys and a persistent host key on the card, so the fingerprint is stable);
(4a) opt-in `param.conf` edit; (5) hands off to `/appfs/script/start.sh`; then in the background (3) decloud when
flag `akam/decloud` is set (blackhole routes `0.0.0.0/1` and `128.0.0.0/1`, LAN kept) and (4b) a report of the local
ports and `param.conf` values. Logs go to `akam/akam_boot.log` on the card. `enable_local.sh` does **not** run an RTSP
server: the stock app has one (UNVERIFIED, PLAN section 9), the script enables and checks it.

## Anti-brick rules (the camera must never be bricked)

1. On-camera scripts write **only to `/tmp` (tmpfs) and the SD card**. The one exception is the opt-in
   `param.conf` edit (`akam/enable_param_conf`), which writes `/config` (jffs2, not firmware) after copying the file to
   the card, with a temp file + `mv` + `sync`; `akam/restore_param.sh` puts the original back.
2. No `nvram set`, no `/dev/mtd*` or `/dev/mtdblock*` access, no `flash_erase`/`flashcp`/`dd` to a device, no `fw_setenv`,
   no `yy_upgrade`, never the stock OTA files (`/tmp/rmem_blk/yy_upgrade.bin`, nvram `YYOS_UPGRADE`). A static test
   (`tests/rot2k_overlay_test.sh`) fails CI if any on-camera file contains such a pattern outside comments. The
   old automatic flash dump of the rescue `hack.sh` is gone: backups are a deliberate step, never every boot.
3. **Removing the card returns the camera to stock.** `akam_boot.sh` always reaches the hand-off to `start.sh`: no
   `set -e`, every step in its own subshell, slow steps in the background, an incomplete card still boots (tested for
   each step failing).
4. The card never carries an upgrade or factory trigger: `yy_debug.ini` is always `mode=debug` (never `mode=factory`),
   `fac_av130.zip`, `*.bin` and `yy_upgrade*` files are never copied, and `build_card.sh` refuses a target that
   holds `fac_av130.zip` or a `yy_upgrade*` file (warns for other firmware-like files).
5. **Never mtd0** (U-Boot, secure boot: unrecoverable brick without a programmer). Phase 1 writes no flash.
6. **Never accept the in-app update (V14.1.7).** A newer YYOS build may close the `mode=debug` hook. Decloud blocks
   the OTA download (it needs the cloud) but **only while the card is in**: without the card the camera is stock and
   can reach the cloud. Defence in depth: also block the camera's Internet access in your router (LAN-only VLAN or a
   firewall rule), and keep the Tuya app away from it.

## Security: telnet is ON by default

The overlay (and the rescue card) start a **passwordless root telnet shell on :23** and set the root password to
`root` (RAM only), so that a card can always rescue a camera. Anyone on your LAN can get root while the card is in.
Turn it off with `build_card.sh ... --no-telnet`, or by creating an empty file `akam/no_telnet` on the card (delete
the file to turn it back on). SSH (Dropbear, port 22) is public-key only (ed25519, no password authentication compiled
in). The SSH host key and your `authorized_keys` live on the card (FAT: no file permissions): treat the card like a
key, and do not share an image of it. Never put the camera on a network you do not trust with the card in.

## Residual risk of the opt-in `param.conf` edit

`akam/enable_param_conf` is the only way the card writes outside `/tmp` and the card. The edit is atomic on jffs2
(new file next to the old one, then `rename`, so a power cut leaves either the old or the new file; if the partition
is full the old file stays) and the original is on the card first. What it cannot rule out: a **well-formed but
unwanted** value (for example, if `rtsp_on_off`/`onvif_on_off` do not mean what the third-party sources say, UNVERIFIED)
that makes `yy_cam` misbehave **also without the card**. Recovery is `restore_param.sh` from the card (SSH/telnet) or
the flash backup. Test with the card in and a way to reach it before relying on this flag; the default is no write.

## Unverified facts

Every fact taken from a third party or from `strings` of the stock binary is marked `# UNVERIFIED (PLAN section 9)` in
the scripts and must be confirmed on the live camera (see the validation checklist): RTSP/ONVIF ports and URLs,
`param.conf` keys, interface name, passwd/shells workarounds, that decloud stops all cloud traffic.
