# Network, time and security

This file covers how the camera stays on the network (Wi-Fi supervisor, DHCP host name), how it learns the time (opt-in
NTP), how it is administered (key-only SSH, per-camera keys, debug flag), how Telnet and FTP are kept off, and which
ports are open by default.

Status: **hardware-validated** on two cameras: clean boot, SSH key login, Telnet and FTP closed, ports 22, 554 and 8091
only with no UDP listener, opt-in NTP, DHCP host name. **host-tested only**: the Wi-Fi supervisor under injected
faults (it runs in the flashed image but no fault was injected), the per-camera key change over SSH, Telnet staying off
after a Wi-Fi reconnect (never tested on a camera).

## 1. Open ports by default

| Port | Protocol | Service | Default |
|---|---|---|---|
| 22 | TCP | Dropbear SSH | open |
| 554 | TCP | RTSP | open |
| 8091 | TCP | Control port | open (read-only from the network) |
| 68 | UDP | DHCP client (`udhcpc`) | open (client) |
| 80 (configurable) | TCP | ONVIF HTTP | closed until ONVIF is enabled |
| 3702 | UDP | WS-Discovery | closed until ONVIF is enabled |
| 21, 23, 24 | TCP | FTP, Telnet, maintenance Telnet | **closed** (never opened in normal operation) |
| 123 | UDP | NTP client | outbound only, only when NTP is enabled; no listener |
| MQTT broker port | TCP | MQTT client | outbound only, only when MQTT is enabled |

`tools/hw_tests/akam_hwcheck.sh` (run on the camera over SSH, read-only) checks TCP listeners, UDP sockets
(`/proc/net/udp` and `udp6`: only port 68, and 3702 with ONVIF on; ephemeral client ports 32768 to 60999 are listed as
information) and a process allow-list: `init syslogd klogd wpa_supplicant udhcpc watchdog ak_rtsp_wrapper ak_rtsp akam_*
dropbear sh sleep drop_caches`, plus the short-lived tools Akam's own scripts call (`ntpd timeout nice ps grep logger
kill cat date wc tail sync mv`). `msgserver` and `cmdserver` are a warning ("vendor daemon, review"); `anyka_ipc`, Telnet
and FTP are failures.

## 2. Wi-Fi supervisor (`akam_net.sh`)

Started detached by the wrapper. It must never delay or break `ak_rtsp`.

- Does nothing at all when `/etc/config/wpa_supplicant.conf` is absent (wired or unconfigured camera).
- Every 30 s checks that `wpa_supplicant` for `wlan0` and `udhcpc` for `wlan0` run and that `wlan0` has an IPv4 address.
- Healthy: it resets its failure state. Otherwise it repairs, escalating:
  - a daemon missing: start it (`wpa_supplicant` first, then `udhcpc` with the host name from `akam_hostname.sh show`);
  - both run but no IPv4 address: the first attempt only waits (the supplicant may be roaming); then it restarts
    `udhcpc`; from the 4th attempt (about 7 minutes) it restarts `wpa_supplicant` too.
- Between two attempts it waits 1, 2, 4, 8, then 10 checks (30 s up to 5 min), so a router that is off for an hour
  costs one cheap check every 5 minutes, not a restart storm.
- BusyBox applets only (no `pgrep`, `tr`, `mktemp`). Log `/tmp/akam_net.log` (100 lines, trimmed to 50) and syslog.
- Wi-Fi power save is not changed (the default of the vendor driver stays).

## 3. Host name and DHCP

- `/usr/sbin/akam_hostname.sh`: `set NAME`, `show`, `status` (`effective`, `configured`, `default`, `kernel`), `reset`,
  `apply`. Stored in `/etc/config/akam/hostname` (mode 0644, written atomically, then `sync`).
- Rules: RFC 1123 single label, 1 to 32 characters of `A-Za-z0-9-`, no leading or trailing `-`, not all digits, stored
  lowercase. A stored name that fails the rules is ignored.
- Default: `akam-XXXX`, where `XXXX` is the last 4 hex digits of the `wlan0` MAC (lowercase); `akam` when the MAC is
  unreadable.
- The wrapper runs `apply` at every start (`hostname <effective>`) and passes the name to `udhcpc -x hostname:<name>`.
  The patched stock `ethernet.sh` does the same through `show`.
- DHCP is **not** restarted when the name changes: `udhcpc` removes the address when it starts, which would cut the
  stream and the SSH session. The router sees the new name at the next boot. The installer offers a power cycle.
- The name is also the ONVIF scope `name/<hostname>`, the MQTT default base topic and client id, and `sys.hostname`.
  ONVIF `SetHostname` writes the same file.
- The control port cannot set the host name (read-only key).

## 4. Time (`akam_ntp.sh`)

The camera has no RTC and boots in the past. NTP is **off by default**: no file, no helper, no network traffic.

| Command (over SSH) | Effect |
|---|---|
| `set SERVER [SERVER [SERVER]]` | Save 1 to 3 servers (host names or dotted IPv4 addresses) in `/etc/config/akam/ntp` and start |
| `off` | Remove the setting and stop the helper |
| `status` | Servers, the camera clock, whether the helper runs, last log line |
| `run` | The loop; started by the wrapper at boot only when the file exists |

- File: one `server=NAME` line per server, at most 3, LF only, mode 0600. A name is letters, digits, hyphens and dots and
  cannot start with a hyphen (it can never be taken as an option by `ntpd`). Anything else is refused (exit 2).
- `run` uses BusyBox `ntpd -n -q -p S ...` in one-shot mode, killed after 60 s by `timeout` when that applet exists, at
  `nice -n 10`. It is not a daemon: no resident process, no UDP socket kept open, no listener for the port audit. The
  camera crystal drifts about a second in 6 hours, enough for time stamps and the ONVIF window.
- Until the clock is 2024 or later it retries after 15, 30, 60, 120, 300 and then 600 s (the network may still be coming
  up); once set it syncs again every 6 hours.
- The clock is stepped by `ntpd`; a step during a viewing session shifts the RTCP wall clock once (the stream itself uses
  the monotonic clock).
- Log `/tmp/akam_ntp.log` (100 lines). Exit codes: 0 ok, 1 failure, 2 usage or invalid server.

## 5. SSH (`akam_ssh.sh`, Dropbear)

### Server

- Dropbear 2026.94 built as a multi-call binary (`dropbearmulti`, links `dropbear` and `dropbearkey`) with
  `tools/dropbear/localoptions.h`: public-key authentication only, no password authentication (also no PAM, no password
  from the environment), **ed25519 keys only** (RSA, ECDSA, DSS and security-key types compiled out), key exchange
  `curve25519-sha256` only, ciphers chacha20-poly1305 and AES-CTR, no forwarding, no `authorized_keys` options, no SFTP.
- Started as `dropbear -r <hostkey> -D <keydir> -p 22 -P /tmp/dropbear.pid -T 3 -E`: at most 3 authentication tries per
  connection, the log (`-E`) goes to `/tmp/akam_ssh.log`.
- Only `root` logs in. A dedicated 0700 directory serves as root's HOME so Dropbear's path-permission walk succeeds on the
  read-only root file system: the helper copies `authorized_keys` into a private tmpfs directory `/tmp/akam_home` and
  bind-mounts a private `passwd` whose root entry has that HOME (the real file is untouched), and a private `/etc/shells`
  that lists `/bin/sh` when the stock file would reject it. Dropbear reads the key list from that copy.
- Host key: `/etc/config/dropbear/dropbear_ed25519_host_key`, generated on first boot with `dropbearkey`, mode 0600.
- Every failure path only logs and returns 0: SSH can never block or stop `ak_rtsp`. Anti-duplicate lock
  `/tmp/akam_ssh.lock`; pid file `/tmp/dropbear.pid`; `akam_lockdown.sh` restarts a dead Dropbear within about 60 s.
- Log `/tmp/akam_ssh.log` is trimmed in place (the inode is kept; Dropbear holds it with `O_APPEND`) to the last 50 lines
  when it passes 100 lines or 64 KiB; `akam_lockdown.sh` trims it again every minute. No key material is logged.

### Key provisioning

No key is embedded in the image. `/etc/config/dropbear/authorized_keys` (mode 0600, directory 0700, root-owned) comes
from one of:

1. **The microSD card.** The helper looks for `akam/authorized_keys` on the card where it is already mounted
   (`/proc/mounts`: `/dev/mmcblk0p1` or `/dev/mmcblk0` on a directory, for example `/mnt` or the toolkit's `/tmp/sd`),
   and only when the card is mounted nowhere mounts it read-only on `/tmp/akam_sd`. A deprecated fallback folder
   `camyn/authorized_keys` from early images is accepted and logged as deprecated.
   - Validation: only lines `ssh-ed25519 <base64> [comment]` are accepted (no options, no `command=`); a file with no
     valid line is ignored and the existing list stays.
   - **Import with hash.** The MD5 of the validated list is kept in `/etc/config/dropbear/.card_keys_hash`. The card is
     imported only when its keys changed since the last import, so a card left in the camera does not undo a key
     change made over SSH. A card with other keys replaces the list; that is the recovery route.
   - The file on the card is never modified or deleted.
2. **A key change over SSH** (`akam_authkeys.sh`, uploaded to RAM by the Go assistant, never installed):
   - `add` (key on stdin): append the key unless already there (idempotent).
   - `remove`: delete every line with that key; **refused** (`AUTHKEYS=REFUSED_LAST_KEY`, exit 3) when no other ed25519
     key would remain.
   - `card`: read-only, reports whether a provisioning card is present.
   - Input must be one well-formed `ssh-ed25519` line (else `BAD_KEY`, exit 2). Keys are compared on the base64 part only.
   - The new list is built in a temporary file in the same directory (mode 0600), checked (expected key count, at least
     one, the key present or gone), then moved into place atomically with `sync` before and after. The tmpfs copy that
     Dropbear reads is replaced the same way; if the two copies differ before a change the result is
     `AUTHKEYS=OUT_OF_SYNC` and nothing changes (a power cycle fixes it).
   - Result lines: `AUTHKEYS=added|present|removed|absent|REFUSED_LAST_KEY|BAD_KEY|OUT_OF_SYNC|WRITE_FAILED|ADDED_BUT_HOME_FAILED|REMOVED_BUT_HOME_FAILED`
     and `CARD=present|absent|unknown`.

### Per-camera keys (installer behaviour)

By default all cameras of one user share one key pair `~/.ssh/akam`. Each camera can instead use its own key
(`~/.ssh/akam-<name>`) or any ed25519 key. The change is made over SSH: add the new public key, log in again with it,
and only then remember it for that camera and ask (default No) whether to remove the old key. If anything fails the
camera still accepts the old key. Private keys are never read, copied or put on a card; a file that contains
`PRIVATE KEY` is refused.

### Host keys on the PC

The assistant pins the camera host key in its own `known_hosts` (in the state directory). A key that differs is a hard
error. If a remembered camera got a new address and presents the same host key, one yes/no (default No) re-pins it under
the new address.

## 6. Telnet and FTP lockdown

Two layers, so a missed launch path is still caught.

**Layer 1: build-time patch of `sbin/service.sh`** (`patch_service_sh.sh`, host tool; deterministic and idempotent):

1. Every non-comment line that mentions `telnetd`, `utelnetd`, `ftpd`, `bftpd` or `vsftpd` (also `tcpsvd ... ftpd`) is
   replaced by `: # AKAM-DISABLED: <original text>` (the no-op keeps the shell syntax valid).
2. Every non-comment line that runs an SD hook (mentions `_ht_ap_mode.conf`, `hack.sh`, `/mnt/.../hostapd`,
   `/mnt/telnet.sh`, or runs `tf_update.sh`) gets the guard `[ -f $AKAM_MAINT_FLAG ] &&` (inside the condition for an
   `if` line, a prefix otherwise). `tf_update.sh` runs the stock `system_upgrade` on every `/mnt/*.bin` at each boot, so
   without the guard any inserted card could reflash the camera.
3. Before the first guarded hook one line records the flag in RAM: `[ -f <flag> ] && : > /tmp/akam_maintenance`
   (stock `service.sh` unmounts the card before the lockdown runs, so the lockdown reads this marker).
4. No other line changes. Exit codes: 0 patched or already patched; 1 usage or I/O; 2 a matching line has a shape the
   script refuses to guess (continuation, compound command, `elif`/`while`/`case`) or the result fails `sh -n`, nothing
   written; 3 no Telnet/FTP launch line found (the build treats this as blocking unless
   `AKAM_ALLOW_UNPATCHED_SERVICE=1`).

**Layer 2: runtime `akam_lockdown.sh`** (started by the wrapper):

- Each round takes one `ps` snapshot and sends SIGTERM (then SIGKILL on the next round if the pid is still there) to every
  process whose command line has a token with the base name `telnetd`, `utelnetd`, `ftpd`, `bftpd`, `vsftpd`, or the
  SD-hook respawn scripts `telnet.sh` and `hack.sh` (the scripts are killed first so they cannot restart a daemon).
- Schedule: every 5 s for 2 minutes (24 rounds), then every 60 s forever. Every slow round it also launches
  `akam_ssh.sh start` detached (a no-op when Dropbear runs).
- It never touches `ak_rtsp`, `dropbear`, `wpa_supplicant`, `cmdserver`, `msgserver` or `akam_*`, nor itself (exclusion
  list checked first). No `pkill` or `killall`. It opens no socket.
- It keeps its own anti-duplicate lock (`/tmp/akam_lockdown.lock`, stale locks taken over).

**Maintenance flag.** The file `AKAM_MAINTENANCE` at the root of the microSD card (the stock firmware mounts the card on
`/mnt`) enables the SD hooks and the maintenance Telnet for first install and recovery. While the flag (or its RAM marker)
is present, the lockdown kills nothing and keeps looping; removing the card or the file resumes the killing at the next
round. The former flag name `CAMYN_MAINTENANCE` is a deprecated alias still honoured. With the flag any `*.bin` on the card
can reflash the camera and nothing is signed: this is a physical-access risk, documented as such.

The hardware check measures the exposure window with `akam_hwcheck.sh --boot-window [N]` (samples the listening ports
every second). The window at boot is believed to be seconds; it has not been measured.

## 7. Debug flag (`akam_debug.sh`)

`akam_debug.sh on [MINUTES]` (1 to 1440, default 60), `off`, `status` talks to the control port through
`ak_rtsp ctl`. It sets the RAM-only debug flag described in [control-port.md](control-port.md): remote peers may write,
read `LOG` lines and see `net.ssid` and `net.mac`. Off at every start, off by itself after the delay.

## 8. Secrets

| Secret | Where | Protection |
|---|---|---|
| Wi-Fi password | `/etc/config/wpa_supplicant.conf` | Written with `umask 077`, mode 0600 by Akam's tools |
| RTSP Digest hash | `/etc/config/akam/rtsp_auth` | 0600 |
| Clear-text stream password (ONVIF) | `/etc/config/akam/credentials` | 0600; refused by the reader when readable by others; opened with `O_NOFOLLOW` |
| MQTT broker password | `/etc/config/akam/mqtt_pass` | 0600; refused when readable by others |
| SSH host key | `/etc/config/dropbear/` | 0600 |

All of these are inside a CONFIG backup. Secrets never appear in `argv`, logs, state files or the control port. There is
no root password on the camera for SSH (password authentication is compiled out); the serial console remains physical
access.
