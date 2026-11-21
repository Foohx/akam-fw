# On the camera: LSC Smart Indoor 1080p (`lsc-indoor-1080p-ak3918`)

What runs on the camera, in order, who starts it, and what Akam keeps, replaces or removes. Facts about stock
files come from the stock APP and rootfs dumps and from [how it works](../../how-it-works.md); anything marked
**unverified** was not checked on a camera. The doc is checked by `tests/on_camera_docs_test.sh`: every Akam path in
backticks must exist in `tools/release/image_files.txt` or be a build output; a stock path is written
`/path` (stock) and is not checked.

Conventions: the APP partition (`mtd7`) is mounted on `/usr`, so an image path such as sbin/akam_ssh.sh is /usr/sbin/akam_ssh.sh on the camera.
Akam never writes anything except `mtd7` (installer) and files under `/etc/config/akam/` (jffs2 `mtd6`, on user
action only). Everything else below writes RAM (`/tmp`).

## 1. Boot chain

| Step | Who | What | Akam |
|---|---|---|---|
| 1 | U-Boot (`mtd0`) | loads the kernel from `mtd4` | stock, never written |
| 2 | kernel + `/sbin/init` (stock) | mounts rootfs (`mtd5`), then APP on `/usr` and CONFIG on `/etc/config` | stock |
| 3 | `/usr/sbin/main.sh` (stock) | boot orchestration; starts `klogd -n &` | **patched at build**: `klogd` line replaced by `ak_rtsp klog` (filtered kernel log, falls back to `klogd`) |
| 4 | `/usr/sbin/service.sh` (stock) | telnet, FTP, SD-card hook, `drop_caches.sh` loop | **patched** by `patch_service_sh.sh`: telnet/FTP/SD-hook lines disabled; the build also slows `drop_caches.sh` from 5 s to 60 s |
| 5 | `/usr/sbin/ethernet.sh` (stock) | DHCP with a fixed host name | **patched** by `patch_ethernet_hostname.sh`: name from `akam_hostname.sh` |
| 6 | `/usr/sbin/sensor_driver.sh` (stock) | insmod `ak_isp`, `sensor_gc20c3`, `ak_venc_adapter`, `ak_venc_bridge`, `ak_pcm`; copies `/tmp/sensor_isp.conf` | kept |
| 7 | `/usr/bin/watchdog` (stock) | reads `/usr/local/_ht_process.ini`, starts each process, feeds the hardware watchdog, respawns | kept; the list is replaced |
| 8 | `/usr/sbin/ak_rtsp_wrapper.sh` | the single entry of `_ht_process.ini` (`process_0`), runs where `anyka_ipc` ran | Akam |
| 9 | `/usr/bin/ak_rtsp` | the one application: ISP, H.264, audio, RTSP, ONVIF, MQTT, control ports | Akam |

Stock names in rows 3 to 7 are the stock APP's own script names (**unverified** for exact file names on the second
board revision `3215672.2`). `cmdserver` and `msgserver` (stock helpers) are kept: removing them stalls the boot.

## 2. Akam files in the image

| File | Started by | Why | Reads / writes | Ports |
|---|---|---|---|---|
| `/usr/local/_ht_process.ini` | stock watchdog (`/usr/bin/watchdog` (stock)) | one process only (`ak_rtsp_wrapper.sh`, delay 0, no ONVIF flag) instead of the Tuya list | none | none |
| `/usr/sbin/ak_rtsp_wrapper.sh` | stock watchdog | respawn backoff (state in `/tmp/akam_respawn`, log `/tmp/akam_wrapper.log`), hostname, starts the helpers below, runs `ak_rtsp` | reads `/etc/config/akam/ntp`; RAM only | none |
| `/usr/bin/ak_rtsp` | the wrapper | video, audio, RTSP, ONVIF, MQTT, control, signed API, day/night, LEDs, `klog`, `--api-key` | settings under `/etc/config/akam/` (written only when a user changes a value, atomic, mode 0600); logs in RAM | 554/tcp RTSP, 8091/tcp control (loopback writes), 8092/tcp signed API (only while an API key exists), ONVIF tcp 80 default and 3702/udp WS-Discovery (opt-in), MQTT client out (opt-in) |
| `/usr/sbin/akam_ssh.sh` | wrapper, background | key-only Dropbear on :22 | reads `/etc/config/dropbear/authorized_keys`, host key in `/etc/config/dropbear/` (generated once) | 22/tcp |
| `/usr/bin/dropbearmulti` (+ links `/usr/bin/dropbear`, `/usr/bin/dropbearkey`) | `akam_ssh.sh` | SSH server and key tool, built with the time32 shim | see above | 22/tcp |
| `/usr/sbin/akam_lockdown.sh` | wrapper, background | second layer: keeps telnet/FTP dead if anything starts them | none | none (kills daemons) |
| `/usr/sbin/akam_net.sh` | wrapper, background | supervises `wpa_supplicant` and `udhcpc` on `wlan0`, growing delay between repairs | `/etc/config/wpa_supplicant.conf` (read) | none |
| `/usr/sbin/akam_hostname.sh` | wrapper (`apply`), `ethernet.sh`, user | host name | `/etc/config/akam/hostname` (user action only) | none |
| `/usr/sbin/akam_ntp.sh` | wrapper (`run`, only if `/etc/config/akam/ntp` exists), user | opt-in time sync, the camera has no clock | `/etc/config/akam/ntp` (user action only) | 123/udp out |
| `/usr/sbin/akam_rtsp_auth.sh` | user over SSH | RTSP/ONVIF Digest login | `/etc/config/akam/rtsp_auth`, `/etc/config/akam/credentials` (user action only) | none |
| `/usr/sbin/akam_onvif.sh` | user over SSH | ONVIF on/off | `/etc/config/akam/onvif` (user action only) | see ONVIF above |
| `/usr/sbin/akam_mqtt.sh` | user over SSH | MQTT on/off | `/etc/config/akam/mqtt`, `/etc/config/akam/mqtt_pass` (user action only) | none |
| `/usr/sbin/akam_debug.sh` | user over SSH | lets the network write on :8091 for N minutes | RAM only | none |
| `/usr/local/akam/LICENSE.dropbear` and the other `LICENSE.*` | nothing | licence texts that must ship with the binaries | none | none |
| `/usr/local/akam/MANIFEST.txt` | nothing | the kit's manifest, for support | none | none |

Hardware check script `scripts/hwcheck/akam_hwcheck.sh` is not in the image (installer side only).

## 3. Removed or replaced stock parts

| Stock | Fate |
|---|---|
| `anyka_ipc` (Tuya app: cloud, video, audio, IR, LED) | removed, replaced by `/usr/bin/ak_rtsp` |
| Tuya entries of `_ht_process.ini` | removed, one entry left |
| telnet, FTP launch lines in `service.sh`, SD-card hook | disabled at build, and killed at run time by `akam_lockdown.sh` |
| `klogd` | replaced by `ak_rtsp klog` |
| the `drop_caches.sh` loop | kept, slowed from 5 s to 60 s (RAM only) |
| kernel, drivers, U-Boot, rootfs, `cmdserver`, `msgserver`, watchdog | kept, untouched |

## 4. Flash writes

| When | What | Partition |
|---|---|---|
| install or update (user) | the whole APP image, read back and verified | `mtd7` |
| a user changes a setting | one small atomic file in `/etc/config/akam/` | `mtd6` (jffs2) |
| first boot with SSH keys | the Dropbear host key | `mtd6` |
| normal running | nothing: state, logs, counters and respawn data live in RAM | none |

No periodic flash write exists (rule: no recurring flash writes in normal operation).

## 5. Open ports (defaults)

22 (SSH), 554 (RTSP), 8091 (control, read-only from the network). Opt-in: 8092 signed API, ONVIF HTTP and 3702/udp.
No UDP listener by default. Never expose these to the Internet.

## See also

[How it works](../../how-it-works.md), [architecture spec](../../../specs/architecture.md), [control API](control-api.md),
[flash and restore](../../manual/flash-and-restore.md), [template for a new camera](../TEMPLATE-new-camera.md).
