# On the camera: LSC Smart Rotatable Camera 2K (`lsc-rotatable-2k-ak3918av130`)

What runs on the camera, in order, who starts it, and what Akam keeps, replaces or removes. Stock facts come from the
user's own backup (offline reverse engineering, **RE**) or from the real camera (**HW**, akam-rot-01); anything else
is marked **unverified**. Deeper evidence: [stock firmware reference](stock-firmware-reference.md),
[card-less appfs](cardless-appfs.md), [decloud findings](decloud-findings.md). The doc is checked by
`tests/on_camera_docs_test.sh`: an Akam path in backticks must exist in the sources that build the image
(`firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh`, `overlay/`); a stock path is written `/path` (stock) and is not checked.

Two ways to run Akam on this camera: **card-less** (the appfs partition `mtd3` carries the payload; current mode) and
the **SD overlay card** (recovery and override: it wins whenever it is inserted).

## 1. Boot chain, card-less (normal)

| Step | Who | What | Akam |
|---|---|---|---|
| 1 | U-Boot (`mtd0`), kernel (`mtd1`) | boots Linux 4.4.302, mounts rootfs (`mtd2`) | stock, never written |
| 2 | `/etc/init.d/rcS` (stock) | insmod cfg80211, mac80211, usbcore, `ak_hcd`; mounts appfs on `/appfs`, config (jffs2) on `/config`; runs `/appfs/script/init.sh &` | kept |
| 3 | `/appfs/script/init.sh` (stock, patched) | reads `/mnt/sdcard/yy_debug.ini` first | **one branch changed**: mode `normal` or no card starts `/appfs/akam/akam_flash_boot.sh`; `debug` and `factory` stay stock; a missing launcher falls back to `start.sh` |
| 4 | `/appfs/akam/akam_flash_boot.sh` | builds a tmpfs tree shaped like the overlay card and bind-mounts it over `/mnt/sdcard`; one-time Dropbear host key in `/config/akam`; starts SSH; hands off to `start.sh`; starts `decloud.sh on` | Akam |
| 5 | `/appfs/script/start.sh` (stock) | loads all drivers (video, NPU, motor, misc), MAC, VM tunables; runs `/appfs/bin/yy_cam &` when nvram `STATUS=active` | kept |
| 6 | `/appfs/bin/yy_cam` | the Tuya application (3.6 MB) | **removed**, replaced by a 3-line launcher of `own_app_stub.sh` (no stock app and no cloud OTA client in the image) |
| 7 | `/appfs/akam/own_app_stub.sh` | what `yy_cam` did that nothing else does: feeds the watchdog (`busybox watchdog -F -t 5 -T 20`), Wi-Fi GPIO 10 + `atbm6132c.ko`, `wpa_supplicant`, `udhcpc`, health check (no network in 120 s: feeder killed, hardware reset), hostname, optional NTP; then starts the supervisor | Akam |
| 8 | `/appfs/akam/own_app_run.sh` | supervisor: checks the app sha256 against its BUILDINFO, runs it from RAM, backoff restart (gives up after 5 exits in 10 min) | Akam |
| 9 | `/appfs/akam/bin/akamd` | the one application: ISP, H.264 (2 streams), RTSP, NPU person detection, PTZ, tracking, ONVIF, MQTT, signed API, LEDs, night, button | Akam |

Stock `yy_upgrade` (OTA), `wpa_supplicant`, drivers, ISP and sensor files stay in the image byte for byte
(checked by a full `unsquashfs` diff, see cardless-appfs section 1). Nothing downloads `yy_upgrade.bin` any more.

Recovery rule: `init.sh` looks at the card **before** any Akam code. A card with `mode=debug` and
`start_sh=/mnt/sdcard/akam_boot.sh` runs the card's code instead, so a broken payload cannot lock the camera out.

## 2. Boot chain, SD overlay card (recovery or override)

| Step | Who | What |
|---|---|---|
| 3 | `/appfs/script/init.sh` (stock) | `mode=debug` in `/mnt/sdcard/yy_debug.ini` runs `start_sh` instead of `start.sh` |
| 4 | `/mnt/sdcard/akam_boot.sh` | rescue (passwordless telnet :23, tmpfs shadow), SSH, optional param.conf edit, with flag `akam/own_app` bind-mounts a `/tmp` copy of `own_app_stub.sh` over `/appfs/bin/yy_cam` (RAM), boot counter (3 unfinished boots: stock `yy_cam` returns), hands off to `start.sh &`, then decloud and verify in the background |
| 5 to 9 | as above | the stub and app come from the card (`/mnt/sdcard/akam/...`) |

Rescue-only card: `/mnt/sdcard/yy_debug.ini`, `hack.sh` and `telnetd` in `rescue/` (a root telnet shell with no password prompt, the root password is set to `root` in RAM; nothing else).

## 3. Akam files

Image side (`/appfs/akam/`, read-only squashfs; the card has the same names under `/mnt/sdcard/akam/`):

| File | Started by | Why | Reads / writes | Ports |
|---|---|---|---|---|
| `/appfs/akam/akam_flash_boot.sh` | `init.sh` | card-less boot (section 1, step 4) | writes `/tmp`; `/config/akam/dropbear_ed25519_host_key` once | none |
| `/appfs/akam/own_app_stub.sh` | `yy_cam` launcher | watchdog, Wi-Fi, health, NTP, supervisor start | reads `/config/akam/wpa.conf` (never printed); RAM only | none |
| `/appfs/akam/own_app_run.sh` | the stub | supervisor, parses `akam.conf` into options | log `/tmp` (size capped, RAM); reads `/config/akam/akam.conf` | none |
| `/appfs/akam/bin/akamd` | supervisor | the application; also `--api-key` tool mode | reads/writes `/config/akam/` files (`api_keys`, `ptz.conf`, `zones`, `mqtt`, `rtsp.conf`, only on user action); `/dev/tymotor`, `/dev/pcmC0D0c`, sysfs GPIO; NPU model from `/appfs` | 554/tcp RTSP, 8092/tcp signed API (only while an API key exists), ONVIF tcp 8000 + 3702/udp (opt-in), MQTT client out (opt-in) |
| `/appfs/akam/bin/akam-sntp` | `akam_ntp.sh` | one-shot SNTP (BusyBox has no `ntpd`) | sets the clock | 123/udp out |
| `/appfs/akam/akam_ntp.sh` | the stub, only with `/config/akam/ntp` | shared with the 1080p | `/config/akam/ntp` | none |
| `/appfs/akam/akam_hostname.sh` | `akam_net.sh` | shared with the 1080p | `/config/akam/hostname` (user action) | none |
| `/appfs/akam/akam_net.sh` | the stub, user | hostname, Wi-Fi band and country, dead-man revert | `/config/akam/hostname`, `/config/akam/net` (user action only) | none |
| `/appfs/akam/akam_api_key.sh` | user over SSH | create, list, revoke API keys | `/config/akam/api_keys` (0600) | none |
| `/appfs/akam/start_ssh.sh` | `akam_flash_boot.sh` | key-only Dropbear | reads `/config/akam/authorized_keys` | 22/tcp |
| `/appfs/akam/dropbearmulti` | `start_ssh.sh` | SSH server and key tool | host key in `/config/akam` | 22/tcp |
| `/appfs/akam/decloud.sh` | `akam_flash_boot.sh`, only with flag `/config/akam/decloud` | blackhole routes `0.0.0.0/1`, `128.0.0.0/1` (WAN cut, LAN kept) and a 30 s keeper that restores them | none (routes in RAM) | none |
| `/appfs/akam/common.sh` | sourced by the others | log and flag helpers | RAM log | none |
| `/appfs/akam/busybox` | stub, scripts | static BusyBox with `sha256sum` and `watchdog` | none | none |

Card only (not in the flash image): `/mnt/sdcard/akam_boot.sh`, `/mnt/sdcard/akam/enable_local.sh` and
`/mnt/sdcard/akam/restore_param.sh` (opt-in edit of the stock app's `/config/param.conf` to turn its local RTSP and
ONVIF on, with a backup and an undo), `/mnt/sdcard/akam/akam_ptz` (motor tool), `/mnt/sdcard/yy_debug.ini`.

## 4. Files and partitions

| Where | Content | Written |
|---|---|---|
| `mtd3` appfs (squashfs xz) | stock appfs plus `/appfs/akam/` | only by `firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh` (dry run by default, needs `AKAM_I_HAVE_RECOVERY=yes` and `--execute`) |
| `mtd4` config (jffs2, 256 KiB) | `/config/akam/*` user files; stock `/config/param.conf`, Tuya data (never read) | only on a user action (setting change, key, one-time host key). Never periodically |
| `mtd0` uboot, `mtd1`, `mtd2`, `mtd5` factory | stock | never written (U-Boot: unrecoverable without an SPI programmer) |
| `/tmp` (RAM) | logs, supervisor state, the app copy, the `/tmp/akamsd` tree | every run, no flash cost |

Watchdog: HW-verified that killing a BusyBox feeder resets the camera about 20 s later; that is the "reset me" path.

## 5. Stock programs that stay or go

| Stock | Fate |
|---|---|
| `yy_cam` (Tuya app, ports 6668, 7000, optional 8554/5000) | removed from the image |
| `yy_upgrade` (stock OTA tool) and the OTA code in `init.sh` | kept, inert: no `yy_upgrade.bin` is ever downloaded |
| `wpa_supplicant`, `udhcpc`, drivers (`atbm6132c.ko`, `ak_npu`, `tymotor`), rootfs, kernel | kept |
| Tuya credentials under `/config/tuya` | untouched, never read |

## 6. Ports (defaults)

22 (SSH, key only, needs `authorized_keys`), 554 (RTSP, main stream0, sub stream1), nothing else until an
API key (8092), ONVIF (8000, 3702/udp) or MQTT is switched on (not confirmed by a port scan of the card-less image). The RTSP streams have no password until one is set.

## See also

[Template for a new camera](../TEMPLATE-new-camera.md), [control API](control-api.md), [net settings](net-settings.md),
[PTZ findings](ptz-findings.md), [flash test plan](../../archive/rot2k-flash-test-plan.md), [UART recovery](uart-recovery.md).
