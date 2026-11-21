# Network settings of the 2K: hostname and Wi-Fi band

Script: `firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/akam_net.sh`, plus the shared
`src/scripts/akam_hostname.sh` reused UNCHANGED (validation, atomic store, kernel `hostname`; installed
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
- runtime `set`/`off`: store, set the kernel name, then **stop and restart udhcpc** with the new name (only if the name
  changed; up to 3 attempts, then the previous name; if no udhcpc starts at all the camera is reset like above, status 1).
  `set`/`off` ignore SIGHUP (udhcpc inherits it) and refuse with status 4 while a Wi-Fi change runs. SIGUSR1 (renew) and
  SIGUSR2 (release) cannot be used alone: the name sent is a command-line option fixed when udhcpc starts. The restart
  does a fresh DISCOVER; udhcpc's deconfig step may drop the address for about a second (an open SSH session survives only
  if the same address comes back).

## Wi-Fi country and band

`/config/akam/net`: `wifi_country=XX|none` (two capitals, default `none` = no country), `wifi_band=2.4|5` (default
**2.4**). There is no `auto`: any other stored value (a legacy `auto`, garbage, an empty value) reads as 2.4, CRLF
residue is stripped, and a stored band 5 without a country also reads as 2.4. Written only by the script, atomically,
and only AFTER a change proved healthy.

```
akam_net.sh wifi show | render | set country XX|none | set band 2.4|5 | fallback [save]
```

- **Band 2.4 (default) = today's proven behaviour.** The rendered `/tmp/wpa.conf` is a byte-identical copy of the user's
  `wpa.conf` (no country line, no key, no comment; a test asserts `cmp` equality). The country is saved but has no effect
  on 2.4. The source is the `wpa.conf` next to the script when there is one (card mode), else `/config/akam/wpa.conf`
  (so a stale file in `/config` cannot win over the card's).
- **Band 5** needs a country (`set band 5` without one, or `set country none` while on 5, is refused with status 2).
  Render = the user's conf byte for byte, then a blank line and two GLOBAL keys: `country=XX` and
  `freq_list=5180 5200 5220 5240` (channels 36-48). No network block is edited, ssid/psk lines are never touched. The
  global `freq_list` restricts the frequencies wpa_supplicant scans (it is not a BSS-selection filter of its own; a
  network with its own `freq_list`/`scan_freq` keeps it). Why only 36-48: no DFS, allowed in every domain, and nl80211
  may reject a WHOLE scan request if one frequency of the list is unsupported by the driver or the country; widening
  to 52-64 / 100-140 / 149-165 needs the camera check below first. The render is refused (status 1, `/tmp/wpa.conf` left
  as is; `set` refuses before touching anything) if a `network={` is not closed by a lone `}` line (or the file has blob
  sections), because the appended keys would land inside a block.
- **Which keys wpa_supplicant 2.10 really parses** (evidence, `/appfs/bin/wpa_supplicant`, ARM ELF): `strings` shows no
  standalone `freq_list`, but that is string tail-merging by the linker: the text is the tail of
  `initial_freq_list`. The key tables prove it. Pointer tables of 28-byte entries (global) and 32-byte entries
  (network) were read from the binary: the global table holds `... ap_vendor_elements, ignore_old_scan_res,
  freq_list, initial_freq_list, scan_cur_freq ...` (the `freq_list` name pointer is `0x8d2cc` = `initial_freq_list` + 8),
  the network table holds `... auth_alg, scan_freq, freq_list, ht, vht, ht40 ...` (same pointer). So `freq_list` is a
  valid GLOBAL key and a valid NETWORK key, `country` is a global key (table entry at `0x7c59c`), `scan_freq` a
  network key; an unknown key is what would make it refuse the conf, none is rendered. `initial_freq_list` is only the
  first-scan list, not used.
- **Runtime change with a dead-man** (`set`): the CLI validates, then detaches a worker (the SSH session rides on wlan0 and
  would die; the worker ignores SIGHUP) and prints `pending`. The worker keeps a copy of the RUNNING conf
  (`/tmp/wpa.conf.prev`), renders the new one, restarts wpa_supplicant (stock command line) and udhcpc (address flushed
  and verified gone, deleted by hand if `ip addr flush` left it), then waits up to 60 s for a healthy link: carrier 1
  (else operstate up), an IPv4 on wlan0 AND a route over wlan0 (the default route is not required, decloud replaces
  it). Healthy: the value is saved. Failure triggers: no link within 60 s; wpa_supplicant or udhcpc fails to start;
  wpa_supplicant is gone 3 s after its start or later while waiting (depending on the build a refused conf makes it exit
  before or after it daemonizes; both orders are handled): these revert **at once**, without waiting 60 s.
  Revert order: (1) the running conf is put back BYTE FOR BYTE and wpa_supplicant + udhcpc restarted; (2) if that fails
  and the saved band was 5, the stock 2.4 bytes (and band 2.4 is saved); (3) if still no working link, **last resort**:
  wpa_supplicant is started on the stock bytes up to 3 rounds if it is not running, and if the link is still not healthy
  the camera is RESET the same way the stub's `fail` does (SIGKILL of the watchdog feeder, pid in
  `/tmp/akam_feeder.pid`, else `reboot`). The saved file always holds the old value or 2.4, so the boot recovers; this
  also undoes the kernel regulatory domain that `country=XX` set (re-rendering 2.4 does not). Exit status 3. Log:
  `/tmp/akam_net.log` (RAM). One change at a time (lock, status 4, also for `hostname set/off` and `fallback`; a lock
  directory without a pid gets a 3 s grace, then it is stale, as in `take_lock`).
- **Pid safety**: `stop_pid` checks `/proc/PID/cmdline` (NUL turned into spaces, so `busybox watchdog` matches on its
  argv[1]) before signalling, and stops EVERY matching process (pidfile plus all `pidof` hits); a stale or recycled
  pidfile falls through to `pidof`.
- **`wifi show`** prints `country=`, `band=` (saved) and `running=` (the band of the conf wpa_supplicant was last
  started with). After a boot fallback WITHOUT save they differ; `set band 5` then retries instead of answering
  `unchanged`.
- **Result of the last `set`**: first word of `/tmp/akam_net.res` (`pending`, `ok`, `refused`, `failed`, `reverted`),
  shown by the control API as the read-only state `net.apply`.
- **Do not change the band while the router is restarting** (or any time the Wi-Fi is not healthy): the dead-man would
  judge the router, not the new band, revert, and in the worst case reach the last resort (camera reset).
- **Last resort** waits for health with the dead-man budget after its stock restart (a lease needs time), not one sample.
- **Country change while on 2.4**: saved at once, nothing restarted (the rendered conf does not change); the command
  says so. Same value again: `unchanged`, nothing written.
- **Boot safety**: with a saved band 5 the stub falls back to the stock 2.4 bytes (`wifi fallback`, restarts
  wpa_supplicant + udhcpc, waits for a busy lock instead of failing): at once and PERSISTENTLY (`fallback save`, band 2.4
  saved, country kept) if wpa_supplicant will not start or exits within 3 s, since the band itself is broken; after half
  of its 120 s health budget with no IPv4 + default route for THIS boot only (no save: after a power cut the router may
  just be slower than the camera). Only then the existing reset path applies. Band 2.4 has nothing to fall back to.

### Camera checklist before trusting band 5 (nothing here was run on the camera)

1. Over SSH, with the card at hand: `wifi set country XX`, `wifi set band 5`, then `cat /tmp/akam_net.log`.
2. `iw` is not installed: look at `dmesg | tail` (regulatory / channel messages of atbm6132c) and `/sys/class/ieee80211/*`
   and `/sys/class/net/wlan0/` for the channel list the driver reports; confirm 5180-5240 are accepted and nothing
   logs a rejected scan (`nl80211` scan request refused / `-EINVAL`) while `freq_list` is set.
3. Only after that consider widening `F5` in `akam_net.sh` (52-64, 100-140 DFS, 149-165), one group at a time.

## Registry rows (not wired yet)

`app/akam2k_netreg.[ch]`: `net.hostname` (string 1-32), `net.wifi_country` (2 capitals), `net.wifi_band`
(`2.4|5`, default 2.4), all apply mode live, writable, HTTP + MQTT exposed, `persist = 0` (the script files are the source of
truth). The hooks call a backend installed by the caller (`a2k_netreg_set_backend`): the app never spawns (anti-brick
scan), so the future wiring supplies a backend that runs `akam_net.sh`. The check hooks refuse what the script would
refuse. Adding these files changes `app/source_hash.sh`: rebuild the app dist (`make poc3-dist`) before the next
`build_card.sh` / `build_appfs.sh` (the binary itself is unchanged, the rows are not linked into it).

## Deployment

- SD overlay card: rebuild with `build_card.sh` (copies `akam_net.sh`, `akam_hostname.sh`, new stub); the files in `/config/akam`
  are read in card mode too.
- Card-less image: the scripts are baked in `/appfs/akam`, so the **appfs image must be rebuilt** (`appfs/build_appfs.sh`)
  and reflashed (or hot-updated) to get them. NOT done here: it needs the stock mtd3 backup and the app dist; no flash
  was attempted. Until then the running camera keeps `akam-rot2k` / band 2.4, and the scripts can be tried from a card.
- Try by hand over SSH (card or flashed): `sh /mnt/sdcard/akam/akam_net.sh hostname set cam-salon`, then
  `... wifi set country FR`, `... wifi set band 5`, and `cat /tmp/akam_net.log` after a minute. Keep the SD card at hand.
