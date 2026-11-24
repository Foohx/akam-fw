# Phase 1 validation checklist (LSC Rotatable 2K, `akam-rot-01`)

> **Superseded (2026-11-24).** The first-boot card was validated long ago and the 2K now runs from its flash. The source of truth for what each camera does today is [docs/features.md](../features.md); the old to new names are in `tools/dev/path-map.txt`. Kept for its reasoning and findings only; do not read its status as current.

> Archived document (2026-11): historical, kept for its findings. Paths and status may predate the repository reorganisation (old to new table: `tools/dev/path-map.txt`); current docs are in [docs/README.md](../README.md).

Copy of the PLAN section 5 checklist with the exact commands. One unit, a user "go" for every boot. Replace
`192.0.2.50` with the camera's address (the router's DHCP list shows it). Everything marked UNVERIFIED in the scripts
is confirmed or corrected here. Record every result; nothing on this page writes flash.

**Rules for the first boot.** The first hardware boot of the overlay is done with the **telnet rescue available**
(the overlay card has it by default; do not build it with `--no-telnet`). Never accept the in-app update V14.1.7.
If anything looks wrong: remove the card and power-cycle, the camera is stock again. Prepare the card as in the
[README](../../firmware/lsc-rotatable-2k-ak3918av130/README.md) (`build_card.sh <dir> --authorized-keys <your .pub> --decloud`;
add `--enable-param-conf` only if the RTSP/ONVIF ports are closed in the first run). Keep the flash backup safe.

- [ ] **0. Confirm telnet root works BEFORE testing anything else.**
  `telnet 192.0.2.50` (a shell, no password; `root`/`root` on UART). Then `cat /proc/mtd` (6 partitions) and
  `cat /mnt/sdcard/akam/akam_boot.log`. If telnet fails, stop here and fix it with the rescue card.
- [ ] **1. Card boots, SSH key login works, cloud unreachable, camera on the LAN.**
  `ssh -i ~/.ssh/id_ed25519 root@192.0.2.50 'uname -a; id'`; password login must be refused:
  `ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no root@192.0.2.50` fails.
  Host key fingerprint stable across two boots: `ssh-keyscan -t ed25519 192.0.2.50 | ssh-keygen -lf -` (compare).
  On the camera: `cat /mnt/sdcard/akam/akam_boot.log` (every step logged, `handed off to /appfs/script/start.sh`).
- [ ] **2. RTSP.** Port, resolution, fps, codec of `/stream0` (main) and `/stream1` (sub):
  `ffprobe -rtsp_transport tcp -hide_banner rtsp://192.0.2.50:554/stream0` and `.../stream1`
  (add `-show_streams -select_streams v` for fps and codec); play it: `ffplay -rtsp_transport tcp rtsp://192.0.2.50:554/stream0`.
  If the port is closed, the log's `listening TCP ports` line says what is open; then try `--enable-param-conf`
  (writes the jffs2 config partition after a backup to `akam/backup/param.conf.orig`; undo with
  `sh /mnt/sdcard/akam/restore_param.sh` and reboot).
- [ ] **3. ONVIF.** Device service answers, record the exact URL and credentials:
  ```sh
  curl -s -m 5 -H 'Content-Type: application/soap+xml' \
    -d '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/></s:Body></s:Envelope>' \
    http://192.0.2.50:5000/onvif/device_service
  ```
  Then `GetCapabilities`, `GetProfiles`, `GetStreamUri` (media service address from `GetCapabilities`), and a snapshot
  (`GetSnapshotUri`, then `curl -o snap.jpg <uri>`). The repo's `tools/hw_tests/onvif_check.py` can be adapted.
- [ ] **4. PTZ** moves via ONVIF (`ContinuousMove`/`Stop` on the PTZ service) and via the native path; record
  pan/tilt ranges and step counts (`grep -A4 '\[ptz\]' /config/param.conf`; read only).
- [ ] **5. Reboot WITHOUT the card** returns to untouched stock behaviour: power off, remove the card, power on;
  the Tuya app sees the camera again, `nmap -p 22,23,8554,5000,6668 192.0.2.50` shows only the stock ports (6668).
  Then put the card back and confirm the overlay returns.
- [ ] **6. RAM/CPU headroom** with the services on (budget gate): `ssh root@192.0.2.50 'free; top -b -n1 | head -15'`,
  while one RTSP client is streaming.
- [ ] **7. NPU / AI detection, read-only check.** With decloud active: `lsmod | grep npu`;
  `grep -E 'npu|svp|cnn' /proc/$(pidof yy_cam)/maps | head`; `dmesg | grep -i npu`. Walk in front of the camera and
  look for detection lines in `yy_cam` output/logs (`ls /tmp/*.log; logread 2>/dev/null | tail -50`), compare motion-only
  vs person. Is the result exposed locally (ONVIF `GetEventProperties`, RTSP metadata, a log)? Record the detector
  targets (person/pet/vehicle) and the NPU RAM/CPU cost. No writes. Feeds the Phase 2 decision.
- [ ] **8. Decloud proof.** From the router/PC, confirm no WAN connection for 30 min (router connection list, or
  `tcpdump -ni <mirror-iface> host 192.0.2.50 and not net 192.0.2.0/24` on a mirror/AP if available); on the camera
  `netstat -tn` shows no non-LAN peer; `ping -c3 8.8.8.8` fails while `ping -c3 <router-lan-ip>` works; the Tuya app shows
  the camera offline. `cat /mnt/sdcard/akam/akam_boot.log | grep decloud` shows both blackhole routes.
- [ ] **8b. Decloud long run (after 24 h, no reboot).** `ip route` still shows `blackhole 0.0.0.0/1`, `blackhole 128.0.0.0/1`,
  `224.0.0.0/4 dev wlan0`, the default route and the LAN /24 (plus yy_cam's `239.255.255.250`);
  `ps | grep 'decloud.sh keeper' | grep -v grep` shows exactly one keeper and `cat /tmp/akam_decloud_keeper.pid` matches;
  `grep 'decloud' /mnt/sdcard/akam/akam_boot.log | grep keeper` shows only a few change lines (a `restored` line after each
  Wi-Fi restart is expected, a steady stream is not); `netstat -tn` shows no non-LAN peer; router connection list shows no WAN
  flow from the camera. Optional: `ip route del 224.0.0.0/4 dev wlan0`, wait 35 s, the route is back and one log line added.
  See [decloud-findings.md](../porting/lsc-rotatable-2k/decloud-findings.md).
- [ ] **9. Anti-brick spot checks.** `ls /mnt/sdcard` shows no `fac_av130.zip`, `*.bin` or `yy_upgrade*`;
  `cat /mnt/sdcard/yy_debug.ini` is `mode=debug`; `ls /tmp/rmem_blk 2>/dev/null` shows no `yy_upgrade.bin`.

**Exit criteria:** a repeatable card build, this checklist green on `akam-rot-01`, an as-built spec drafted in
`docs/porting/lsc-rotatable-2k/`.
