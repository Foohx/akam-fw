# Network settings of the 2K: hostname and Wi-Fi band

Script: `firmware/lsc-rotatable-2k-ak3918av130/overlay/akam/akam_net.sh`, plus the shared
`tools/firmware_patch_templates/akam_hostname.sh` reused UNCHANGED (validation, atomic store, kernel `hostname`; installed
beside it as `akam/akam_hostname.sh` by `build_card.sh` and `appfs/build_appfs.sh`, linked into the card-shaped tree by
`appfs/akam_flash_boot.sh`). Files live in `/config/akam` (jffs2, written on user actions only). Nothing here touches
flash partitions, nvram or the user's `wpa.conf`. No camera test yet: everything below is verified on the host with fakes
(`tests/rot2k_overlay_test.sh`, section "net settings").

## Hostname

`/config/akam/hostname`, one RFC 1123 label (1-32 of A-Za-z0-9-, no leading/trailing `-`, not digits only), stored in
lowercase; absent or invalid = `akam-rot2k` (the 1080p default `akam-XXXX` from the MAC is not used here).

```
akam_net.sh hostname show | set NAME | off | apply
```

- boot (`own_app_stub.sh` step 3a): `hostname apply` sets the kernel name (was `YYOS`), and `udhcpc` gets
  `-x hostname:NAME -p /tmp/udhcpc.pid`.
- runtime `set`/`off`: store, set the kernel name, then **stop and restart udhcpc** with the new name. SIGUSR1 (renew) and
  SIGUSR2 (release) cannot be used alone: the name sent is a command-line option fixed when udhcpc starts. The restart
  does a fresh DISCOVER; udhcpc's deconfig step may drop the address for about a second (an open SSH session survives only
  if the same address comes back).

## Wi-Fi country and band

`/config/akam/net`: `wifi_country=FR` (two capitals, default FR), `wifi_band=auto|2.4|5` (default auto). Written only by
the script, atomically, and only AFTER a change proved healthy.

```
akam_net.sh wifi show | render | set country XX | set band auto|2.4|5 | fallback
```

- **Rendering** (boot, and every change): `/tmp/wpa.conf` (0600) = the user's `wpa.conf` byte for byte, then an appended
  block with the global key `country=XX` and, for 2.4 / 5 only, `freq_list=` (2.4: 2412-2472, channels 1-13; 5: 5180-5320,
  5500-5700, 5745-5825). A later global key wins in wpa_supplicant; the `network={}` blocks are never edited and the file is
  never printed. wpa_supplicant here is v2.10 (strings of `/appfs/bin/wpa_supplicant` show `country`, `initial_freq_list`,
  `scan_freq`, `RECONFIGURE`, `SET`, but there is no wpa_cli on the camera). UNVERIFIED on the camera: whether the atbm6132c
  driver honours the regulatory country (it is expected to stop skipping 5 GHz channels as no-IR) and whether the global
  `freq_list` filters scans in this build. If `freq_list` is not honoured, the fallback is `scan_freq=` inside the network
  blocks of the RENDERED copy only.
- **Runtime change with a dead-man** (`set`): the CLI validates, then detaches a worker (the SSH session rides on wlan0 and
  would die) and prints `pending`. The worker renders the new conf, restarts wpa_supplicant (stock command line) and
  udhcpc (address flushed first, so an IPv4 afterwards proves a new lease), then waits up to 60 s for an IPv4 on wlan0 AND
  a route over wlan0 (the LAN route; the default route is not required, decloud replaces it). Healthy: the new value is
  saved. Not healthy, or wpa_supplicant fails to start: **revert** (re-render the previous values, restart both again), log
  it, exit status 3; the saved file is unchanged, so a power cut mid-test also keeps the old value. Wi-Fi is never left
  stopped. Result and reasons: `/tmp/akam_net.log` (RAM). One change at a time (lock, status 4).
- **Boot safety**: with a saved band other than auto, if the stub sees no IPv4 + default route within half of its 120 s
  health budget it runs `wifi fallback` (renders band auto, restarts wpa_supplicant + udhcpc, keeps the saved value) and
  waits the rest; only then the existing reset path applies.

## Registry rows (not wired yet)

`app/akam2k_netreg.[ch]`: `net.hostname` (string 1-32), `net.wifi_country` (2 capitals), `net.wifi_band`
(`auto|2.4|5`), all apply mode live, writable, HTTP + MQTT exposed, `persist = 0` (the script files are the source of
truth). The hooks call a backend installed by the caller (`a2k_netreg_set_backend`): the app never spawns (anti-brick
scan), so the future wiring supplies a backend that runs `akam_net.sh`. The check hooks refuse what the script would
refuse. Adding these files changes `app/source_hash.sh`: rebuild the app dist (`make poc3-dist`) before the next
`build_card.sh` / `build_appfs.sh` (the binary itself is unchanged, the rows are not linked into it).

## Deployment

- SD overlay card: rebuild with `build_card.sh` (copies `akam_net.sh`, `akam_hostname.sh`, new stub); the files in `/config/akam`
  are read in card mode too.
- Card-less image: the scripts are baked in `/appfs/akam`, so the **appfs image must be rebuilt** (`appfs/build_appfs.sh`)
  and reflashed (or hot-updated) to get them. NOT done here: it needs the stock mtd3 backup and the app dist; no flash
  was attempted. Until then the running camera keeps `akam-rot2k` / auto, and the scripts can be tried from a card.
- Try by hand over SSH (card or flashed): `sh /mnt/sdcard/akam/akam_net.sh hostname set cam-salon`, then
  `... wifi set country FR`, `... wifi set band 5`, and `cat /tmp/akam_net.log` after a minute. Keep the SD card at hand.
