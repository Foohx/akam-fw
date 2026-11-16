# Parity matrix: 1080p versus the Rotatable 2K

Directive: the LSC Smart Indoor 1080p (`src/ak_rtsp`) reaches the same level of features and remote control as the
LSC Rotatable 2K (`firmware/lsc-rotatable-2k-ak3918av130/app`, `akam2k_*`), each within its own hardware. Written from
the code of both (dev 6d3a9f4), no camera access. The ordered plan is in [convergence-plan.md](convergence-plan.md) section 7.

Legend. **HW** = validated on a camera (per [testing-status.md](testing-status.md) and the porting docs); **host** = host
tests only (fake hardware), never on a camera. Shared code: `C` = already in `src/common`, `2K` = only in the 2K app
(`firmware/lsc-rotatable-2k-ak3918av130/app/`, portable, move candidate), `1080` = only in `src/ak_rtsp`.
Cells marked "(check)" were read from docs, not re-proven.

| Feature / control | 2K status | 1080p status | Hardware constraint | Shared code | Gap / action |
|---|---|---|---|---|---|
| RTSP main stream | `rtsp://IP:554/stream0`, H.264, `akam2k_rtsp.c`, HW | `rtsp://IP:554/stream0`, 1080p, `rtsp.c`, HW | both exist | C: `rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtsp_busy rtp_clock tx_pace net_tx h264_params` | none (URLs harmonized). The two session loops stay (frame source differs) |
| Sub-stream `/stream1` (640x360) | yes, own encoder channel (`akam2k_cap.c`, `akam2k_stream.c`), HW | none | 1080p: one `/dev/venc-chn0` opened by `venc.c`; a second channel costs DMA pool and CPU, unproven | none | probe a second venc channel on akam-01 (HW, small, after the CPU fix). If refused: N/A, documented |
| RTSP clients | thread per client (`akam2k_rtsp.c`), `rtsp.clients` | single slot, 2nd client gets 503 (`rtsp_busy.c`), HW | 1080p RAM: 777 KB encoder ring + 256 KiB TCP queue per client | C `rtsp_busy` | keep one slot on the 1080p (go2rtc in front). Not a parity item |
| RTSP Digest auth | `rtsp_auth.c`, host | same, HW | none | C `rtsp_auth` | none |
| Audio capture (mic track) | `akam2k_audio.c`, opt-in, HW | `audio.c`, HW | PCM: vendor lib vs raw ioctls | C: `g711 audio_jitter audio_vad` | none, drivers stay per model |
| Two-way audio (talk) | none: speaker path unresolved (`docs/porting/lsc-rotatable-2k/audio-findings.md`) | ONVIF backchannel, `talk.c audio_out.c rtp_in.c`, HW | 2K: speaker device not found | C pure parts; 1080: `talk rtp_in audio_out` | 2K gap, not a 1080p one. When solved, `talk`+`rtp_in` go to common |
| ONVIF + WS-Discovery | none (RTSP only) | `onvif*.c soap.c http_srv.c wsd.c`, HW (34/34 checks, ODM) | 2K has PTZ (ONVIF PTZ service is new work) | 1080 only | reverse gap (2K gets opt-in ONVIF, after the registry) |
| MQTT + HA discovery | `akam2k_mqtt.c akam2k_mqttcfg.c`, host | `mqtt_svc.c mqtt_ops.c`, host | none | C: `mqtt mqtt_ha` | no real broker run on either. Entity list generated from the registry at P9 |
| Signed HTTP API :8092 | `akam2k_api.c akam2k_httpd.c`, host only (camera checks A1-A9 pending) | none; text port :8091 (`control.c ctl_gate.c`), HW | gate +56 KB binary, +64 KB RSS (`specs/control-api.md` s9) | 2K: `api_sig api_keys api_limit json_min sha256` (pure) | P1 to P8 |
| Settings registry | `settings_reg.c` + table `akam2k_settings.c` | none; hand-written GET/SET in `control.c` (ae, night, isp, venc, ctl, log, fw, sys.hostname, onvif, mqtt) | none | 2K `settings_reg` | 1080p table with aliases keeping the 8091 names (P7) |
| Events ring (`GET /events`) | `akam2k_apievt.c`, host | none | none | 2K | moves with the API (P8) |
| Audit log | ring 16 KB + 20 persisted writes in `akam2k_api.c`, host | none (`ctl_gate.c` only gates writes) | flash wear: persist rarely | 2K | with the API (P8) |
| API keys | `api_keys.c`, `tools/akam-api-sign.sh`, host | none | none | 2K | P2 move, P8 wire |
| Rate limit | `api_limit.c`, host | `ctl_gate.c` connection gating | none | 2K | P2 move, used at P8 |
| Settings persistence | `akam2k_store.c` (tmp+fsync+rename, skip-if-equal) | `cfg_file.c` atomic write (C); skip-if-equal only in `video_ctl.c` | SPI NOR wear | C `cfg_file` | store helper (section 3 step 6) |
| Status LED | `light_logic.c hal_light.h hal_light_sysfs.c akam2k_leds*.c`: on/off/blink, rules `led.boot/nowifi/person/stream/night/idle`, GPIO 56, HW (on at boot), rules host | `leds.c`: LED-class `state_led`, `status=on|off|auto`, file `/etc/config/akam/leds`, HW | 1080p: LED class, no GPIO; no person/stream state | 2K `light_logic hal_light hal_light_sysfs` | P6: 1080p descriptor `{status, LEDCLASS, state_led}`, rules boot/nowifi/idle only |
| IR LEDs | GPIO 48, `night.ir_led auto|off`, HW | `ledsys.c` + `leds.c` hook, `night.ir_led auto|off`, HW | IR pin lives inside `ircut.c`/`ledsys.c` | 2K `light_logic` | P6 exposes IR as an emitter, keeps `night.ir_led` |
| Day/night | `akam2k_night*.c`, HW | `night.c ae.c`, HW (dusk/dawn long run not done) | different ISPs | C `night_logic` | done for the decision; keys become registry rows (P7) |
| IR-cut | `akam2k_ircut.c` GPIO 24/25 | `ircut.c` GPIO pulses, HW | different pins | none | stays per model |
| Image settings | `akam2k_image.c` (brightness, contrast, sharpness, saturation, wdr, mains_hz) via vendor lib | `imgq.c isp_tune.c isp_blocks.c` (`isp.*` -50..50), RAM on one camera, final image not yet | raw ISP tunnel vs vendor lib | key grammar only | align names/ranges via registry aliases; check `wdr`/`mains_hz` support (HW) |
| Video kbps/gop/fps | live: `akam2k_vlive.c` set_attr (`venc-live-params.md`) | read at start, restart to apply (`video_cfg.c video_ctl.c`) | 1080p kernel may expose the same `'V'` ioctls (hypothesis) | 2K `vlive` state machine (pure-able); `video_cfg` parse half | P10 `hal_venc` + probes on akam-01 |
| QP / rc mode | `min_qp max_qp rc` live (`akam2k_video.c`) | hard-coded 28/43 VBR+ (`venc.c:229-236`); `feature/1080p-qp-tuning` in flight | kernel oopses on wrong rc words (history) | `video_cfg` | after qp-tuning merges: expose keys, defaults identical |
| Person detection | `akam2k_detect.c`, NPU, PoC HW | none | 1080p: no NPU | none | N/A |
| Motion detection | none | none | no known HW block; software diff costs CPU on a board already high | none | not planned; revisit after the CPU fix |
| Zones | `akam2k_zones.c akam2k_zlive.c akam2k_detstate.c`, host | none | needs a detector | 2K `detstate` | N/A until a detector exists |
| Tracking / PTZ | `akam2k_track*.c track_logic.c akam2k_ptz*.c hal_ptz*.c scene_check.c`, motor HW | none | fixed lens, no motor | none | N/A |
| Front button | `button_logic.c hal_input*.c akam2k_button.c`, host only | none | 1080p GPIO unknown, maybe no button (`docs/models.md`) | 2K | find the pin first (HW); absent = N/A |
| Metrics `hal_sys` | `hal_sys.c`, `sys.*` (cpu, load, mem, uptime, wifi), host | `ctl_sys.c` counters only (clients, frames, skipped, auth) | `/proc`, generic | 2K `hal_sys` | P3 move, P4 wire as read-only keys |
| Logs | `akam2k.log`, log endpoint planned | kernel log filter `klog` (C) + log pump on 8091, HW | RAM only | C `klog` | one `log.*` registry row family after P7 (planned) |
| Hostname | `net.hostname` (`akam2k_netreg.c`), host | `akam_hostname.sh`, `sys.hostname` read, HW | none | shell | registry row over the existing file, no new mechanism |
| NTP | opt-in `akam2k_sntp` + `akam_ntp.sh`, HW | opt-in `akam_ntp.sh`, HW on both cameras | none | shell `akam_ntp.sh` | none |
| Decloud | `--decloud` blackhole routes, HW | not needed: no stock cloud client | stock app absent | shell | none |
| Wi-Fi band / country | `net.wifi_band net.wifi_country` (`akam2k_netreg.c`), in flight `integ/wifi-api` | N/A | 1080p radio is 2.4 GHz only | 2K only | N/A; 1080p keeps `net.wifi_rssi` |
| SSH | dropbear, key admin, HW | same, HW | none | `tools/dropbear` | none |
| Update path | hot appfs `--hot` (`cardless-appfs.md` 6c), never run on a camera; card recovery | SD / maintenance flash, `reflash_ssh`, HW (md5 read-back, power cycle) | 8 MiB NOR, card boot on 1080p | none | model-specific, not a parity item |
| Watchdog | capture stall `DEAD_MS` only (`akam2k_cap.c`) | `swdog.c` stall self-exit + wrapper respawn (host, never triggered) | none | 1080 `swdog_st_*` pure half | split `swdog` core (deferred in step 2), 2K adopts |
| Snapshot JPEG | `akam2k_snap.c` | none | 1080p JPEG ioctl unknown | none | optional, after P10 |

## Reading

Top gaps on the 1080p: (1) no signed JSON API / registry / events / audit (text :8091 only), (2) kbps/gop/QP not live and
QP not settable, (3) no LED rules and no `hal_sys` metrics, (4) no sub-stream, snapshot, button (hardware-bound: prove
N/A on the board, do not assume). Reverse gaps (2K lacks): ONVIF, `swdog` beats, `klog` filter, talk path.
Most 2K cells are host-only. Parity means matching the 2K after its own camera pass, so the 2K checklists A1-A9
(`docs/hardware-test-checklists.md`) gate each 1080p port of the same code.
