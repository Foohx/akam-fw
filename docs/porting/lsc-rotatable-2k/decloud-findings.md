# Decloud routes: what can remove them, and the keeper

Offline analysis of the stock rootfs (`/usr/share/udhcpc/default.script`) and strings of `yy_cam`, plus two hardware
observations (2026-11-14). Nothing here was executed on the camera. No vendor file is stored in the repository.

## What can remove our routes

| Actor | Effect on our routes | Confidence |
|---|---|---|
| udhcpc `deconfig` (`ifconfig <if> 0.0.0.0`) | Brings the address down: routes bound to the interface go (LAN subnet, `224.0.0.0/4 dev wlan0`). Blackholes have no device and stay. | High (script read; kernel behaviour) |
| udhcpc `renew`/`bound` | `ifconfig <if> <ip> netmask ...`, then loops `route del -net ... default gw 0.0.0.0` (specific to the default route, errors discarded) and `route add default gw <router>`. It never flushes: blackholes and the multicast route are not named. The script only writes DNS (adds 8.8.8.8 / 4.2.2.2 to resolv.conf; harmless, they are blackholed). | High (script read) |
| `yy_cam` Wi-Fi restart (`killall -q wpa_supplicant`, restart, `yy_net_kill_dhcp`, `udhcpc -i wlan0 -x hostname:YY_CAM`) | Link goes down: every `dev wlan0` route is dropped, including `224.0.0.0/4 dev wlan0`. Confirmed on hardware: after reboot + ~5 min our multicast route was gone while both blackholes remained. | High (observed) |
| `yy_cam` own multicast code (`yy_net_wifi_station_{add,del,exist}_multicast_route`, `yy_net_{add,del}_multicast_route`, string `route add -net 224.0.0.0 netmask 224.0.0.0 eth0`) | Its del function may delete a `224.0.0.0` route. It also adds a more specific `239.255.255.250 dev wlan0` host route (seen on hardware), which the keeper leaves alone. | Medium (strings only, call sites not traced) |
| `route del` / `ip route flush` in `yy_cam` | No `route del` or `flush` string besides the above was found; `/proc/net/route` is read to find the default router. | Medium (strings can hide built commands) |

Net: the blackholes are only at risk if something deletes them by name (not found); the multicast route is lost on every
Wi-Fi restart. DHCP renewals over hours remain unverified on hardware but the script gives no reason for concern.

## Keeper

`decloud.sh on` (flag `akam/decloud`) starts one background `decloud.sh keeper`:

- Single instance: pidfile `/tmp/akam_decloud_keeper.pid`, claimed atomically (noclobber) and validated with `kill -0` plus `/proc/<pid>/cmdline`
  (a stale file or a reused pid is replaced, never killed); re-running `on` does not start a second one.
- Every 30 s (`AKAM_KEEP_SECS`): one `ip route show`; only if `blackhole 0.0.0.0/1`, `blackhole 128.0.0.0/1` or
  `224.0.0.0/4` is missing does it `ip route add` those. The default route, LAN routes and `239.255.255.250` are never touched.
- Logs only changes to the card log (shared trimmed log): `keeper started`, `keeper: routes were missing, restored`,
  `keeper: routes missing and could not be restored` (once until it recovers), `keeper: routes back`.
- Cost: one `sleep` + one `ip` + a few `grep` per 30 s, shell builtins otherwise. Started in the already-background
  decloud step, so it never delays the hand-off to `start.sh`.
- Stops by itself if the flag file disappears; `decloud.sh off` stops it (waiting for an iteration in progress) and removes the routes it added.
- IPv6 is not kept (the kernel has none).

## Hardware results 2026-11-14 (akam-rot-01)

- After reboot: both blackholes + `224.0.0.0/4 dev wlan0` + yy_cam's `239.255.255.250` present; exactly one keeper
  (pid = pidfile); keeper CPU 0.0 %.
- The keeper restored the multicast route on its own at +70 s and +100 s (yy_cam Wi-Fi restarts drop it).
- Manual `ip route del 224.0.0.0/4 dev wlan0` → restored within one tick (≤ 30 s), one log line.
- 24 h run: pending (check keeper log volume, routes, `netstat -tn` for WAN peers).
