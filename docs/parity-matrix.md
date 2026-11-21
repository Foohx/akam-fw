# Parity matrix: 1080p versus the Rotatable 2K

Directive: the LSC Smart Indoor 1080p (`platforms/anyka-ak3918`) reaches the same level of features and remote control as the
LSC Rotatable 2K (`platforms/anyka-ak3918av130`, `akam2k_*`), each within its own hardware. Written from
the code of both (dev 6d3a9f4), no camera access. The ordered plan is in [convergence-plan.md](convergence-plan.md) section 7.

Legend. **HW** = validated on a camera (per [testing-status.md](testing-status.md) and the porting docs); **host** = host
tests only (fake hardware), never on a camera. Shared code: `C` = already in `src`, `2K` = only in the 2K app
(`platforms/anyka-ak3918av130/`, portable, move candidate), `1080` = only in `platforms/anyka-ak3918`.
Cells marked "(check)" were read from docs, not re-proven.

| Feature / control | 2K status | 1080p status | Hardware constraint | Shared code | Gap / action |
|---|---|---|---|---|---|
| RTSP main stream | `rtsp://IP:554/stream0`, H.264, `rtsp_session.c`, HW | `rtsp://IP:554/stream0`, 1080p, `rtsp_session.c`, HW | both exist | C: `rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtsp_busy rtp_clock tx_pace net_tx h264_params` | none (URLs harmonized). The two session loops stay (frame source differs) |
| Sub-stream `/stream1` (640x360) | yes, own encoder channel (`cap.c`, `stream.c`), HW | none | 1080p: one `/dev/venc-chn0` opened by `venc.c`; a second channel costs DMA pool and CPU, unproven | none | probe a second venc channel on akam-01 (HW, small, after the CPU fix). If refused: N/A, documented |
| RTSP clients | thread per client (`rtsp_session.c`), `rtsp.clients` | single slot, 2nd client gets 503 (`rtsp_busy.c`), HW | 1080p RAM: 777 KB encoder ring + 256 KiB TCP queue per client | C `rtsp_busy` | keep one slot on the 1080p (go2rtc in front). Not a parity item |
| RTSP Digest auth | `rtsp_auth.c`, host | same, HW | none | C `rtsp_auth` | none |
| Audio capture (mic track) | `audio.c`, opt-in, HW | `audio.c`, HW | PCM: vendor lib vs raw ioctls | C: `g711 audio_jitter audio_vad` | none, drivers stay per model |
| Two-way audio (talk) | none: speaker path unresolved (`docs/porting/lsc-rotatable-2k/audio-findings.md`) | ONVIF backchannel, `talk.c audio_out.c rtp_in.c`, HW | 2K: speaker device not found | C pure parts; 1080: `talk rtp_in audio_out` | 2K gap, not a 1080p one. When solved, `talk`+`rtp_in` go to common |
| ONVIF + WS-Discovery | none (RTSP only) | `onvif*.c soap.c http_srv.c wsd.c`, HW (34/34 checks, ODM); rows `onvif.enabled` (refused without the stream password), `onvif.state` | 2K has PTZ (ONVIF PTZ service is new work) | 1080 only | reverse gap (2K gets opt-in ONVIF, after the registry) |
| MQTT + HA discovery | `mqtt_bind.c mqtt_cfg.c`, host | `mqtt_svc.c mqtt_ops.c`, host; rows `mqtt.enabled` (flips the file's `enabled=`), `mqtt.state` | none | C: `mqtt mqtt_ha` | no real broker run on either. Entity list generated from the registry at P9 (open) |
| Signed HTTP API :8092 | `api_core api_httpd http_req` (C) bound by `api_bind.c`, host only (camera checks A1-A9 pending) | same shared code bound by `firmware/lsc-indoor-1080p-ak3918/api_bind.c`, opens only with a key, `ak_rtsp --api-key`, host only (checklist B6-B14 pending); text port :8091 unchanged, HW | gate +56 KB binary, +64 KB RSS (`specs/control-api.md` s9): the 1080p registry+API is +79 KB, over the gate, RSS not measured | C: `api_core api_httpd http_req api_sig api_keys api_limit api_events json_min sha256 settings_reg` | **P8 done (host)**; camera pass open |
| Settings registry | `settings_reg.c` + table `settings.c` | `firmware/lsc-indoor-1080p-ak3918/settings.c`: 41 rows (led, night, image, video0, onvif/mqtt switches, state), 8091 keeps its names and the registry names are offered as a fallback; host only | none | C `settings_reg` | **P7 done (host)**. Not rows: ae.*, night thresholds, img.* (8091 only), identity data |
| Events ring (`GET /events`) | `api_events.c`, host | `night`, `setting` events (no person, ptz, button: no such hardware), host | none | C `api_events` | done with P8 |
| Audit log | ring 64 + 20 persisted writes (`api_core.c`), host | same code, file `/etc/config/akam/audit` (writes only, burst 2 then 1 per 30 s), host; `ctl_gate.c` still gates 8091 writes | flash wear: persist rarely | C `api_core` | done with P8 |
| API keys | `api_keys.c`, `tools/akam-api-sign.sh`, host | same, `/etc/config/akam/api_keys`, tool `ak_rtsp --api-key`, host | none | C `api_keys` | done (assistant support later) |
| Rate limit | `api_limit.c`, host | same on 8092 + a flash limiter (burst 6, 1 per 20 s); `ctl_gate.c` on 8091 | none | C `api_limit` | done with P8 |
| Settings persistence | `store.c` (tmp+fsync+rename, skip-if-equal) | modules keep their own files; the API store skips an unchanged file (`firmware/lsc-indoor-1080p-ak3918/api_bind.c`) | SPI NOR wear | C `cfg_file` | store unification (section 3 step 6) still open |
| Status LED | `light_logic.c hal_light.h hal_light_sysfs.c akam2k_leds*.c`: on/off/blink, rules `led.boot/nowifi/person/stream/night/idle`, GPIO 56, HW (on at boot), rules host | `leds.c` on `light_logic` + `led_pat`: LED-class `state_led` as emitter `status`, rules `stream boot nowifi night idle` with patterns (defaults = the old behaviour, oracle-proven), `led.blip`, file `/etc/config/akam/leds`; behaviour HW (old engine), new engine host | 1080p: LED class, no GPIO; no person state | C `light_logic led_pat` | **P6 done (host + oracle)**; visual check B3/B4 open |
| IR LEDs | GPIO 48, `night.ir_led auto|off`, HW | `ledsys.c` + `leds.c` hook, `night.ir_led auto|off` (registry row too), HW | IR pin lives inside `ircut.c` | 2K `light_logic` | the IR pin is NOT an emitter of the engine yet (kept inside `ircut.c`); the row is `night.ir_led` |
| Day/night | `akam2k_night*.c`, HW | `night.c ae.c`, HW (dusk/dawn long run not done); rows `night.mode night.active`, event `night` (host) | different ISPs | C `night_logic` | done for the decision; registry rows and event done (P7, P8) |
| IR-cut | `ircut.c` GPIO 24/25 | `ircut.c` GPIO pulses, HW | different pins | none | stays per model |
| Image settings | `image.c` (brightness, contrast, sharpness, saturation, wdr, mains_hz) via vendor lib | `imgq.c isp_tune.c isp_blocks.c` (`isp.*` -50..50) as rows `image.brightness contrast saturation sharpness denoise` (scale -50..50, told by the schema); `wdr`/`mains_hz` not offered | raw ISP tunnel vs vendor lib | key grammar only | rows done (P7); check `wdr`/`mains_hz` support (HW) |
| Video kbps/gop/fps | live: `vlive.c` set_attr (`venc-live-params.md`) | rows `video0.kbps fps gop min_qp max_qp` saved, apply at the next start (`restart_component`; gop in SECONDS), `video0.cfg_pending` | 1080p kernel may expose the same `'V'` ioctls (hypothesis) | 2K `vlive` state machine (pure-able); `video_cfg` parse half | rows done (P7); live apply is P10 `hal_venc` + probes on akam-01 |
| QP / rc mode | `min_qp max_qp rc` live (`video.c`) | `video0.min_qp max_qp` rows (28/43 default, min < max over the whole PATCH), restart to apply; no `rc` row | kernel oopses on wrong rc words (history) | `video_cfg` | rows done (P7); live and `rc` are P10 |
| Person detection | `detect.c`, NPU, PoC HW | none | 1080p: no NPU | none | N/A |
| Motion detection | none | none | no known HW block; software diff costs CPU on a board already high | none | not planned; revisit after the CPU fix |
| Zones | `zones.c zlive.c detstate.c`, host | none | needs a detector | 2K `detstate` | N/A until a detector exists |
| Tracking / PTZ | `akam2k_track*.c track_logic.c akam2k_ptz*.c hal_ptz*.c scene_check.c`, motor HW | none | fixed lens, no motor | none | N/A |
| Front button | `button_logic.c hal_input*.c button.c`, host only | none | 1080p GPIO unknown, maybe no button (`docs/models.md`) | 2K | find the pin first (HW); absent = N/A |
| Metrics `hal_sys` | `hal_sys.c`, `sys.*` (cpu, load, mem, uptime, wifi), host | `ctl_sys.c` on `hal_sys_sample`: old keys unchanged + `sys.cpu_pct sys.load1_x100 sys.mem_total_kb sys.mem_avail_kb sys.uptime_s net.wifi_lq`, host | `/proc`, generic | C `hal_sys` | **P4 done (host)**; camera read B2 open |
| Logs | `akam2k.log`, log endpoint planned | kernel log filter `klog` (C) + log pump on 8091, HW | RAM only | C `klog` | one `log.*` registry row family after P7 (planned) |
| Hostname | `net.hostname` (`netreg.c`), host | `akam_hostname.sh`, `sys.hostname` read, HW; row `net.hostname` read-only | none | shell | writable row needs the script (the app never spawns): open |
| NTP | opt-in `akam2k_sntp` + `akam_ntp.sh`, HW | opt-in `akam_ntp.sh`, HW on both cameras; rows `net.ntp` (servers) and `sys.time_set` read-only | none | shell `akam_ntp.sh` | writable row needs the script: open |
| Decloud | `--decloud` blackhole routes, HW | not needed: no stock cloud client | stock app absent | shell | none |
| Wi-Fi band / country | `net.wifi_band net.wifi_country` (`netreg.c`), in flight `integ/wifi-api` | N/A | 1080p radio is 2.4 GHz only | 2K only | N/A; 1080p keeps `net.wifi_rssi` |
| SSH | dropbear, key admin, HW | same, HW | none | `tools/dropbear` | none |
| Update path | hot appfs `--hot` (`cardless-appfs.md` 6c), never run on a camera; card recovery | SD / maintenance flash, `reflash_ssh`, HW (md5 read-back, power cycle) | 8 MiB NOR, card boot on 1080p | none | model-specific, not a parity item |
| Watchdog | capture stall `DEAD_MS` only (`cap.c`) | `swdog.c` stall self-exit + wrapper respawn (host, never triggered) | none | 1080 `swdog_st_*` pure half | split `swdog` core (deferred in step 2), 2K adopts |
| Snapshot JPEG | `snap.c` | none | 1080p JPEG ioctl unknown | none | optional, after P10 |

## Status of the 1080p porting (2026-11-17, branch `convergence/p4-p8`)

P4, P6, P7 and P8 are done and host-tested; nothing of them has run on a camera (checklist B1-B14 in
[hardware-test-checklists.md](hardware-test-checklists.md)). P5 (`swdog` split) was skipped on purpose (not needed). Still open: P9 (MQTT from the
registry), P10 (live video and QP), P11 (second stream, button, JPEG), P12 (`log.*` rows), P13 (reverse parity), and the assistant side of the key tool.

## Reading

Top gaps on the 1080p: (1) no signed JSON API / registry / events / audit (text :8091 only), (2) kbps/gop/QP not live and
QP not settable, (3) no LED rules and no `hal_sys` metrics, (4) no sub-stream, snapshot, button (hardware-bound: prove
N/A on the board, do not assume). Reverse gaps (2K lacks): ONVIF, `swdog` beats, `klog` filter, talk path.
Most 2K cells are host-only. Parity means matching the 2K after its own camera pass, so the 2K checklists A1-A9
(`docs/hardware-test-checklists.md`) gate each 1080p port of the same code.
