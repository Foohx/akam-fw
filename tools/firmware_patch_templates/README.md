# Firmware Patch Templates

These files are the patch applied on top of your own stock `app_extracted/`
dump to produce the de-Tuyafied `app_patched/` tree. Used automatically by
`src/ak_rtsp/build_firmware.sh` — see `docs/akam_v1_build.md` for the
full walkthrough. Listed here individually so the diff against stock is easy to read.

| File | Installed as | Purpose |
|---|---|---|
| `_ht_process.ini` | `local/_ht_process.ini` | Single process entry launching the wrapper |
| `ak_rtsp_wrapper.sh` | `sbin/ak_rtsp_wrapper.sh` | Starts the Akam helpers in the background, Wi-Fi, then `ak_rtsp` |
| `akam_ssh.sh` | `sbin/akam_ssh.sh` | Starts Dropbear (ed25519 key login), keys persisted in CONFIG; see `docs/akam_v1_ssh_and_lockdown.md` |
| `akam_lockdown.sh` | `sbin/akam_lockdown.sh` | Runtime safety net that kills telnet/FTP daemons |
| `akam_rtsp_auth.sh` | `sbin/akam_rtsp_auth.sh` | Run over SSH: turns the optional RTSP Digest login on or off (`status`, `set`, `disable`); see the README |
| `akam_hostname.sh` | `sbin/akam_hostname.sh` | Network hostname (`set`, `show`, `status`, `reset`, `apply`); default `akam-XXXX` from the wlan0 MAC; stored in `/etc/config/akam/hostname` |
| `akam_debug.sh` | `sbin/akam_debug.sh` | Run over SSH: debug mode of the control port 8091 (`on [MINUTES]`, `off`, `status`); lets the network write for a while, RAM-only; uses `ak_rtsp ctl` |
| `patch_ethernet_hostname.sh` | host tool, not installed | Build-time patch of `sbin/ethernet.sh`: the DHCP hostname comes from `akam_hostname.sh show` (fallback `akam`) |
| `patch_service_sh.sh` | host tool, not installed | Build-time patch of `sbin/service.sh` (telnet/FTP launch lines, SD hook guard) |

The two sections below describe the first two files. The Akam scripts document themselves in their
headers.

## `_ht_process.ini` → `app_patched/local/_ht_process.ini`

Stock's version has three process entries:

```ini
[process_0]
name = anyka_ipc
...
[process_1]
name = rtspserver
...
[process_2]
name = nvtservice
...
```

This template reduces it to a single entry that launches `ak_rtsp_wrapper.sh` instead
of `anyka_ipc`. `rtspserver`/`nvtservice` are dropped entirely — they're already inert
on stock firmware (gated behind `onvif_switch = 0` in `_ht_sw_settings.ini`, which is
never flipped on), so removing their process entries changes nothing at runtime.

**Do NOT** touch `cmdserver`/`msgserver` — they are not process-list entries (they're
started directly from `service.sh`) and are pure local IPC, not Tuya cloud clients.
Removing them causes a boot stall. See `docs/flash_partition_map_and_rootfs.md`.

## `ak_rtsp_wrapper.sh` → `app_patched/sbin/ak_rtsp_wrapper.sh`

`process_0`'s actual entry point. First starts `akam_ssh.sh` and `akam_lockdown.sh` in the
background (fire and forget, only if present and executable). Then brings up WiFi manually (DHCP) before exec'ing
`/usr/bin/ak_rtsp`, since `ethernet.sh` skips WiFi bring-up whenever
`/tmp/_ak39_factory.ini` is present (its own factory/production-test gate) — relevant
if you keep an `_ak39_factory.ini` on the SD card as a fallback path back to stock
factory-demo RTSP mode.
