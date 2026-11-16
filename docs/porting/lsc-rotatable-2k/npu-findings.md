# LSC Rotatable 2K: AI (NPU) detection findings (offline analysis of the stock app)

Status: offline reverse engineering only (nothing was run on a camera). Same method and conventions as
[ptz-findings.md](ptz-findings.md): addresses are virtual addresses in the stock `/appfs/bin/yy_cam` (ARM32 EABI5,
stripped, dynamic symbols kept; text 0x75cb0, rodata 0x2f3dd8). No vendor code, dump, key or value is reproduced
here; only symbol and string names and addresses. Every claim stays UNVERIFIED in the sense of PLAN section 9 until
the hardware procedure (section 7) is run.

Confidence: HIGH = read directly in the instructions, MEDIUM = inferred from several consistent observations,
LOW = guess.

## 1. Summary

1. **The AI "hd" detector does not depend on the cloud, a Tuya DP, nvram or any param.conf key to RUN** (HIGH).
   `main` calls `yy_alg_init` (0x273858, call at 0x79970) unconditionally, before `yy_cloud_init`. It registers the
   noise, motion (md) and "hd" algorithms; `yy_alg_ak_av130_hd_init` then reads its configuration, creates the SVP2
   channel on the NPU and starts a thread (`Y_ALG_AV130_HD`). The only off switch is the param.conf key
   `[alg] ak_av130_svp2_hd_enable` (default **1** when absent): when 0 the init logs "disabled alg, init failed!"
   (0x27e9dc) and the detector (and the NPU use) never starts. This matches the live fact that `ak_npu` is used by
   `yy_cam` with the cloud blocked and no AI key in param.conf.
2. **"hd" = the vendor's "human detection"** (source path `anyka_av130_human_detection`, HIGH for the name). The
   class set is not visible without the model (target type default 0, SVP2 model type 10; LOW on what exactly it
   classifies: person vs head/upper body). Do not assume pets or vehicles.
3. **Results never leave the process in a usable form.** The detector posts an internal message
   (`send_event_message`, 0x271428) to the Tuya event module, which can (a) push a cloud alarm, (b) start an SD
   "event" recording and a cloud-storage event. There is **no** local export: no file, no socket, no ONVIF event
   (section 4). The only cheap local trace is the stock log (section 5).
4. **ONVIF cannot deliver them (HIGH by static analysis; an indirect call cannot be fully excluded, T3 confirms).** The event service has no PullPoint, announces one static topic, and the
   function that would push a Notify (`onvif_notify`, 0x24b848) has no caller anywhere in `yy_cam`.
5. **`ivs_mt_on_off` is auto motion tracking** (the camera follows a target with the motor): `ivs_mt_*` is read by the
   hd module and the "mt" result goes to `yy_ptz_set_track_info` (HIGH on the code path, MEDIUM-HIGH on the user
   meaning). Its default is 0 and the live value is 0: nothing moves the motor on its own. Keep it 0.
6. **Recommendation:** for Phase 2 use a **log tail** of `yy_cam`'s stdout on the SD card as the event source (expected
   to work with the stock app, no change to the camera apart from one empty file on the card; UNVERIFIED until T4); for Phase 3 run the model ourselves.
   ONVIF events are not viable without patching `yy_cam` (section 6).

## 2. Who enables the hd detector, and with which keys

The configuration store is `yy_conf` (`yy_conf_init` reads `/config/param.conf`, falls back to
`/config/default_param.conf`). Getters: `yy_conf_get_int` (0x2e2b80), `yy_conf_get_str` (0x2e281c); the DP handlers
write through `yy_conf_set_int` (0x2e2cb8). Defaults are literals in the code (HIGH for all lines below).

| Section / key | Default | Meaning |
|---|---|---|
| `[alg] ak_av130_svp2_hd_enable` | 1 | master switch of the hd detector; 0 = never started, no NPU use |
| `[alg] ak_av130_svp2_hd_mode_options` | 0x102 | must intersect 0x102 or init fails ("mismatch between the mode options") |
| `[alg] ak_av130_svp2_hd_skip_frames` | 0 | frames skipped between runs |
| `[dsp] ak_av130_svp2_hd_chn` | 3 | video channel fed to the detector (queue read in the thread, 0x27dd50) |
| `[dsp] ak_av130_svp2_hd_score_threshold` | 750 | |
| `[alg] ak_av130_svp2_hd_model_type` / `_classify_threshold` / `_iou_threshold` / `_target_type` / `_env_mode` / `_conf_mode` / `_handle_id` | 10 / 700 / 3 / 0 / 0 / 0 / 0 | passed to `ak_svp2_*` (ranges checked in libmpi_svp2: threshold 0..1000, IoU 0..10) |
| `[alg] ak_av130_svp2_hd_conf` | `/appfs/resource/model/model_0x0A00000D_V1.00.01.bin` | model path, a local file |
| `[dsp] ivs_hd_enable` | 1 | config word only printed in the log ("** hd enable: %d"); not a gate in the code read |
| `[dsp] ivs_hd_on_off` | 0 | the **alarm** switch of hd (see below) |
| `[dsp] ivs_hd_sensitivity` / `_debounce_delay` / `_max_duration` / `_roi_onoff` / `_roi` | 30 / 30 / 300 / 0 / empty | |
| `[dsp] ivs_hd_record_on_off` | 1 | event recording on a hd event |
| `[dsp] ivs_md_on_off` | **1** | motion alarm: on by default |
| `[dsp] ivs_md_*`, `[alg] ak_av130_svp_md_dev0_*` | see section 4.4 | classic motion detection |
| `[dsp] ivs_mt_on_off` / `ivs_mt_enable` / `ivs_mt_sensitivity` | 0 / 1 / 50 | motion tracking (section 3) |
| `[dsp] ivs_nd_*`, `ivs_vd_*`, `ivs_pd_*`, `ivs_rail_*`, `ivs_intrude_*` | on_off default 0 | noise and other algorithms, not examined |

`ivs_*_on_off` is what the Tuya app calls the alarm switch: `yy_alg_conf_set_alarm_onoff` (0x274790) stores it with
`yy_conf_set_int` and, when switching off, logs "stop hd event when hd alarm off" and stops the running event. The DP
handlers that call it: `yy_cloud_tuya_dp_response_motion_switch` (md), `..._humanoid_filter` (hd, 0x7e708),
`..._motion_tracking` (mt, 0x7f390), `..._hd_record_switch` (0x87470). So the **on/off is DP-driven**: with the cloud
blocked, only a LAN DP write (Tuya LAN, port 6668, needs the local key) or a param.conf edit changes it. The DP
numbers were not decoded here (open item).

Interaction: "stop md event when hd alarm on" (0x3598b8): with the hd alarm ON, motion events are suppressed (the
Tuya "humanoid filter"). With the live config (md alarm 1 by default, hd alarm 0) both detectors run, hd events are
not pushed as alarms.

What reads `ivs_hd_on_off`: not the detector. No read of the on/off word was found in the hd module (only the
printing of `enable`/sensitivity); the gate is in the event consumer below (MEDIUM: the module is 0x27dc14-0x280a04,
searched for loads of its context offsets).

## 3. What "mt" is

`yy_alg_ak_av130_hd_read_conf_ivs_of_mt` (0x27e100) lives in the hd source file: motion tracking is derived from the
SAME SVP2 run. The dispatcher `yy_alg_result_process` (0x273124) maps result kind 0x800 to
`yy_alg_result_process_mt` (0x272824), which calls `yy_ptz_set_track_info` (0x26dad4); `ms_locat_coor` (0x273068)
calls `yy_ptz_set_locate_coor_info` (0x26dcd8). The motion-tracking DP handler also calls `yy_ptz_set_move_onoff`
(0x26a080). So `ivs_mt_on_off=1` would make the camera pan/tilt after a detected person (HIGH on code,
MEDIUM-HIGH on behaviour). Not wanted for Akam, and it competes with `akam_ptz` for the motor: leave 0.

## 4. Where the results go

### 4.1 Internal path (HIGH)

`yy_alg_ak_av130_hd_detect_th` (0x27dd50): loop { take a frame from the channel queue, run SVP2 (0x2801bc), convert
and filter results (0x28062c), sleep 100 ms when no frame }. Result callback -> `yy_alg_result_process` ->
`yy_alg_result_process_hd` (0x271bb0), which logs "##### hd event start msg sended" / "hd event started" (log level
1, tag `ALG`), calls `send_event_message(2, 203, ...)` (0x271428, `yy_msg_send`) and later "hd event stop msg sended".
Kind bits seen in the dispatcher: 1 md, 2 hd, 8 vd, 32 pd, 64 nd, 0x100 rail, 0x800 mt, 0x1000 / 0x2000
intrude / location (the last ones LOW).

### 4.2 The consumer (MEDIUM)

`yy_cloud_tuya_process_event_start` (0x899fc, tag `CLO_TY_EVENT`) maps the event to a Tuya alarm type (hd = 9, md =
0, sound = 13), reads `event_onoff` = `yy_alg_conf_get_alarm_onoff` (0x2754d4) and `record_onoff`
(0x2758c8), logs "event_onoff = %d, record_onoff = %d" and "alarm_info.is_notify = %d" (level 1), then:

- `event_onoff` true: `tuya_ipc_trigger_alarm_without_event` (0x1751a4): the push notification, **cloud only**;
- `record_onoff` true: `tuya_ipc_ss_start_event` (SD card event recording of the Tuya SDK, works offline with
  `[cloud] local_record_switch=1`) and `tuya_ipc_cloud_storage_event_start` (cloud storage, dead offline).

Consequence (MEDIUM): with the cloud blocked, hd detections still start/stop SD event recordings (record default 1)
and still produce log lines; the app "alarm" is lost. A hd snapshot is also taken ("hd snap jpeg failed" string).

### 4.3 What is NOT there (HIGH)

- **No ONVIF event.** `GetEventProperties` template (0x35206c) announces only
  `tns1:RuleEngine/CellMotionDetector/Motion` with `IsMotion`. (The coordinator saw "MotionDetector/Motion": the
  binary spells `CellMotionDetector`; same single topic.) `GetServiceCapabilities` (0x38df84) says
  `WSPullPointSupport="false"`; there is no `CreatePullPointSubscription` string. Only the basic WS-BaseNotification
  `onvif_Subscribe` (0x24b6f4, reference format `http://%s:%d/event_service/%u`) exists, which needs a consumer
  address that the camera POSTs to. The Notify builder (template 0x352818, hard-coded `Rule="TestRule"`,
  `IsMotion=true`) is reached only from `onvif_notify` (0x24b848); no `bl`/`b` and no stored pointer to it exists in
  the binary. The device-service callback `ds_get_motionstatus` (0x242bf4) returns a word of a 0x2dc-byte structure
  at 0x3fb7f0 that nothing else in `yy_cam` writes (MEDIUM). Expectation: Subscribe succeeds, no Notify ever
  arrives, for motion and for human alike.
- No RTSP metadata, no file or unix socket carrying events, no environment variable.

### 4.4 Classic motion detection (`svp_md`, MEDIUM)

`[alg] ak_av130_svp_md_dev0_*` (enable 1, mode_options 1, dev_id 0, fps 5, move_size_max 400, move_size_min 4, four
day/night filter words 0) and `[dsp] ivs_md_*` (on_off 1, sensitivity 0, debounce 30, max_duration 300). The wrappers
`ak_svp_md_init/get_result/...` (0x27bfa4, 0x27c844, ...) and `ak_vpss_md_get_stat` suggest hardware motion statistics
of the VPSS plus a software filter, polled by `yy_alg_ak_av130_md_detect_th` (0x279634, thread
`Y_ALG_AV130_MD`), delivered through the same `yy_alg_result_process_md` (0x2717c4) -> Tuya consumer path. This does
not use the NPU. Its on_off defaults to 1, which is why motion "alarms" exist in stock without any setting.

## 5. Cheap, read-only ways to observe events on hardware

1. **Stdout to the card (HIGH on mechanism).** `yy_sys_loop` (main.cpp, call chain 0x78bf0/0x78bf8) polls, on every
   loop turn:
   - `/mnt/sdcard/yy_sdcard_log` present: redirects the process stdout (`dup2`) to `/mnt/sdcard/sdcard_log.txt`
     ("start sdcard log"); the file is rotated to `sdcard_log.txt.old` at 60 MiB (0x3c00000); removing
     `yy_sdcard_log` restores stdout ("stop sdcard log"). Needs no restart. Handler at 0x78750.
   - `/mnt/sdcard/yy_log_debug` (fallback `/config/yy_log_debug`; read once per loop turn by 0x78218): lines
     `YY_LOG_LEVEL=DEBUG|INFO|WARN|ERROR`, `YY_LOG_TAG=<tag>`, `CLOUD_LOG_LEVEL=...`. Reading the code, the default
     level (file absent) is 1 = DEBUG, so the hd/event lines (level 1) are printed without this file (MEDIUM; if
     the log stays silent, create it with `YY_LOG_LEVEL=DEBUG`). INFO would hide them.
   Lines to look for: `hd event start msg sended`, `hd event started`, `hd event stop`, `start tuya alarm type[`,
   `event_onoff =`, `alarm_info.is_notify`, `md ... event`, and at boot `** hd enable`, `** hd chn_attr model_type`,
   `register alg hd`. `yy_cam` is started by `start.sh` as a plain background job, so its stdout is otherwise
   lost (consistent with the coordinator's note).
2. **Thread names and CPU ticks (HIGH on the names).** The detector threads set their name with `prctl(15)`:
   `Y_ALG_AV130_HD` (0x27dd60), `Y_ALG_AV130_MD`, `Y_ALG_SOUND_DETECT` (shown truncated to 15 characters). On the
   camera: `grep -l Y_ALG /proc/<pid>/task/*/comm`; utime/stime of those tasks in `/proc/<pid>/task/<tid>/stat`.
   `enable_local.sh verify` now logs them (section 8). Comparing two readings tells whether the detector runs and what
   it costs.
3. **Param and NPU state.** `verify` also logs the AI keys of param.conf, the log-switch files, the `ak_npu` module
   line and who holds the NPU node.
4. **New event recordings on the card** after someone walks by (Tuya SD event index), MEDIUM.

## 6. Options for Akam to get person/motion events locally (ranked)

| # | Option | Phase | Verdict |
|---|---|---|---|
| 1 | **Log tail of `sdcard_log.txt`** (lines of section 5.1) parsed on the camera by a tiny shell/BusyBox loop and published to MQTT by the existing bridge, or parsed on the host over SSH | 2 | Expected (UNVERIFIED until T4) to work with the unmodified stock app, no flash write; needs the 60 MiB rotating file on FAT (wear: keep it only while needed, or point the bridge at a short-lived file), depends on the log text staying stable (it is a fixed firmware, so yes). RAM: ~0 (a `tail -f`/`grep` process, < 1 MB). CPU: negligible; the cost is the DEBUG log volume of `yy_cam` itself (measure bytes/min in the procedure). Latency: expected sub-second (unmeasured). Gives start/stop of hd and md, no box/class/score. |
| 2 | **Own app runs the detector** (Phase 3, replaces `yy_cam`): `ak_svp2_*`/`libak_nne` on the model file already in `/appfs/resource/model` (not redistributed), md via the VPSS statistics | 3 | The only route to bounding boxes, scores and a real person class. Cost: reverse the `ak_svp2_*` call sequence (the stock call site is `yy_alg_ak_av130_hd_init_interface`, 0x27f9e8, and the thread at 0x27dd50, readable), RAM = model (985,012 bytes) + NPU buffers (unknown, measure), CPU = the `Y_ALG_AV130_HD` thread ticks measured in the procedure. Frames come from the vendor video libs we must drive anyway. |
| 3 | ONVIF events to MQTT (Subscribe consumer on the PC/Akam) | 2 | **Not viable** while `yy_cam` is stock: no Notify is ever sent (section 4.3). Would need a binary patch of `yy_cam` (not redistributable) or our own ONVIF server in Phase 3. The probe tool exists to prove this on hardware in one minute. |
| 4 | Tuya LAN (6668) DP reports | 2 | LOW: alarms are cloud messages, not DPs; only the switches (motion, humanoid filter, tracking, record) are DPs. Useful to flip `ivs_hd_on_off` at runtime, not to read events. |
| 5 | Flip `[alg] ak_av130_svp2_hd_enable=0` | - | Do not, unless the NPU/CPU cost turns out to hurt other services; it disables detection altogether. |

Recommended path: option 1 as the Phase 2 MVP (and as the measurement tool now), option 2 as the target.

## 7. HARDWARE TEST PROCEDURE (coordinator; read-only first)

Preconditions: the current card build, cloud blocked, SSH works, RTSP/ONVIF on (palier 5 state). `<cam>` is the
camera address on the LAN, `<pc>` the address of the PC as the camera sees it. One person walks in front of the
camera in the steps marked WALK (stay in view 20 s, leave 20 s, repeat 3 times). Abort criteria (any step, then undo
that step and stop): `yy_cam` pid changes, `dmesg | tail` shows NPU/driver errors, free RAM drops below 5 MB
(`free`), the card log grows faster than 20 MB per 5 minutes, or the RTSP/ONVIF streams stop answering.

**T1. Static state (no change).** On the camera: `sh /mnt/sdcard/akam/enable_local.sh verify`, then
`tail -n 8 /mnt/sdcard/akam/akam_boot.log`. Record: `param.conf AI keys`, `stock log switches`, `npu module`,
`npu node open by pid`, `AI detector threads`. Expected (MEDIUM): `Y_ALG_AV130_HD` and `Y_ALG_AV130_MD` threads
exist; the key list shows `dsp.ivs_mt_on_off = 0` (as already known) and no `alg.*` key. If `Y_ALG_AV130_HD` is
missing, the detector is not running (then record `ps`, `dmesg | grep -i npu`, and stop: the claim "runs without
cloud" is wrong).

**T2. Idle vs WALK CPU (no change).** Run `verify` twice, 60 s apart, nobody in view; again 60 s apart during WALK.
Record the utime+stime deltas of each `Y_ALG_*` thread (ticks of 10 ms) and `top -b -n 1 | head -n 12`
(yy_cam share), `free`. This is the NPU CPU cost for the PLAN budget.

**T3. ONVIF events (PC side, no change on the camera).** Credentials are the known ones (user `admin`, empty
password; set `ONVIF_PASSWORD=` empty):
```
T=firmware/lsc-rotatable-2k-ak3918av130/tools/onvif_events_probe.py
python3 $T --host <cam> --user admin caps          # expect WSPullPointSupport=false
python3 $T --host <cam> --user admin properties    # expect one topic, CellMotionDetector/Motion
python3 $T --host <cam> --user admin pullpoint     # expect a refusal
python3 $T --host <cam> --user admin listen --listen-host <pc> --duration 120   # WALK during these 2 minutes
```
Expected (HIGH-ish): `listen: subscribed` then **zero Notify**. Any `notify ... IsMotion=` line would contradict
section 4.3: record it with the timestamps, it makes ONVIF option 3 viable for motion. The PC firewall must let the
camera reach the listener port (`--listen-port N` to pin it).

**T4. Stock log on the card (one empty file, reversible).** From the PC mounting the card, or on the camera:
`: > /mnt/sdcard/yy_sdcard_log`. Wait one minute, check `ls -l /mnt/sdcard/sdcard_log.txt` (it must appear and
grow). Then WALK 2 minutes, and:
```
grep -nE 'hd event|start tuya alarm|event_onoff|is_notify|md.*event|CLO_TY_EVENT' /mnt/sdcard/sdcard_log.txt | tail -n 40
grep -nE 'hd enable|register alg|disabled alg|chn_attr' /mnt/sdcard/sdcard_log.txt | head          # only if the log covers the boot
wc -c /mnt/sdcard/sdcard_log.txt                                                                   # bytes for 2 minutes
```
Record which lines appear for a person (hd and md both? only md?), the delay between entering the view and the
`hd event start` line, and the log volume. Then **stop**: `rm /mnt/sdcard/yy_sdcard_log` (stdout is restored on the
next loop turn), keep `sdcard_log.txt` for the record, delete it afterwards (`rm` it and the `.old`; the log is capped at 60 MiB plus the same again in `.old`, so up to ~120 MiB
on the card until removed). Never leave `yy_sdcard_log` in place after the test. If nothing
shows although T1 found the thread, create `/mnt/sdcard/yy_log_debug` containing `YY_LOG_LEVEL=DEBUG` and repeat.
To see the boot lines (`hd enable`, model type) the file must exist before `yy_cam` starts: create it, reboot with
the card, read the head of the log.

**T5. Only if T4 shows no hd lines at all although the thread runs (opt-in, reversible).** Goal: learn whether the
detector needs `ivs_hd_on_off=1` for events to flow. Use the existing safety net: `akam/backup/param.conf.orig` is the
copy made by `enable_local.sh apply` (palier 5), `akam/restore_param.sh` puts it back. Check first that
`akam/backup/param.conf.orig` exists on the card (otherwise stop: no safety net). Edit by copying `/config/param.conf`
to `/tmp`, setting `ivs_hd_on_off=1` INSIDE the `[dsp]` section (replace the existing `ivs_hd_on_off` line if there is
one, never append a duplicate or a line under another section; diff the copy against the original: exactly one line
may differ), then `cp` it to a temp file in `/config` and `mv` it over `param.conf` (same method as
`enable_local.sh`), reboot with the card, repeat T4, then restore with `restore_param.sh` and reboot. Abort and
restore at once on any criterion above or if the camera does not come back with RTSP up. Do NOT set `ivs_mt_on_off=1` (the motor would start
following people) and do not touch `ak_av130_svp2_hd_enable` except to test the kill-switch (value 0 must make the NPU
use disappear: `npu node open by pid: none`, thread gone).

**T6. Record the outcome** in PLAN section 7 (palier 6): thread names and tick deltas, log lines per event kind and
latency, log bytes per minute, whether any Notify arrived, whether `ivs_hd_on_off` mattered.

## 8. Code changes in this branch

- `overlay/akam/enable_local.sh` `verify`: new read-only `log_ai_state` (AI keys of `[dsp] ivs_*` and
  `[alg] ak_av130_*`, log-switch files, `ak_npu` module line, holders of the NPU node by name, CPU ticks of the
  `Y_ALG_*` threads). Writes only to the card log. Tests in `tests/rot2k_overlay_test.sh` (fixtures `el6`, `el7`).
- `tools/onvif_events_probe.py` (host side, standard library only, no address or credential built in) and
  `tests/rot2k_onvif_probe_test.py` (loopback fake camera, run from the overlay test).

## 9. Open items

- The class set of the model (person only? head/shoulders? face), and what `target_type` selects: needs a run with
  `ak_svp2_*` results or the model format; the log does not show it.
- DP numbers of the humanoid filter, motion switch and motion tracking (for a LAN DP client).
- Exact log text of the md events and the real log volume (T4).
- Whether the hd thread input channel (`ak_av130_svp2_hd_chn=3`) is a low-resolution stream (cost) and its frame rate.
- Whether the `[cloud] local_record_switch` SD event recording really triggers on hd (look at the card index files).
- `tuya_ipc_download_algorithm_model_*` exists: the cloud can push a model, which would change the file named by
  `ak_av130_svp2_hd_conf`; irrelevant while the cloud is blocked.

## Hardware results 2026-11-14 (akam-rot-01, cloud blocked)

- T1/T2: threads `Y_ALG_AV130_HD`, `Y_ALG_AV130_MD`, `Y_ALG_SOUND_DET` run; HD ≈ 5 % CPU (almost all system time, NPU
  driver), same idle vs walking. Stock log shows `register alg noise detect / md / hd` at boot.
- T3: ONVIF `Subscribe` is refused with "Invalid IPv4 Address" whatever the consumer address format; together with the
  static finding (Notify template hard-coded "TestRule", no caller) ONVIF events are considered unusable.
- T4 (`yy_sdcard_log`, 2 min walking in view, user watching a live preview): **no hd/md event line**, with
  `ivs_hd_on_off` absent (0) and with `ivs_hd_on_off = 1` added under `[dsp]` + reboot (then restored, md5-identical).
  Boot log: `ak_svp_md_set_sensitivity: sensitivity:0` → the detection sensitivities/zones are most likely cloud-pushed
  settings (DPs) that default to 0/off without the cloud. Next lead: find where sensitivity comes from (DP handler →
  stored where?) before any further change.
- **Security: the stock SD log (`sdcard_log.txt`) writes the Wi-Fi SSID and PASSWORD in clear** (`tkl_wifi_station_connect`)
  plus Tuya device identifiers and cloud URLs. Never leave `yy_sdcard_log` on the card, never ship or upload that log,
  and any Akam use of it must filter at the source. The SD-log option for Akam events is demoted accordingly.
