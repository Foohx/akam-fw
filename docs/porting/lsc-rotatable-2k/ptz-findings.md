# LSC Rotatable 2K: PTZ findings (offline analysis of the stock app)

Status: offline reverse engineering only (nothing was run on a camera). Every claim is UNVERIFIED in the sense of
PLAN section 9 until the hardware procedure at the end is run. Addresses are virtual addresses in the stock
`/appfs/bin/yy_cam` (ARM32 EABI5, stripped, dynamic symbols kept; text at 0x75cb0) and file offsets in
`ty_driver.ko`. No vendor code, dump, key or value is reproduced here; only symbol names and addresses.

Confidence scale: HIGH = read directly in the instructions, MEDIUM = inferred from several consistent
observations, LOW = guess.

## 1. Summary

1. **ONVIF PTZ cannot be switched on from `param.conf`.** In this build the ONVIF PTZ is not wired at all. Three
   independent gaps, each enough to explain the symptom (HIGH):
   - the "this device has PTZ" flag that makes the ONVIF stack create a PTZ node is never set by the code that
     builds the ONVIF configuration;
   - the PTZ callbacks of the ONVIF layer are never filled in;
   - the exported `ds_ptz_control` is an empty stub.
   No `param.conf` key, nvram key or file presence is read on that path.
2. The motor itself is simple: `yy_cam` drives `/dev/tymotor` with five ioctls (section 4). A tiny on-camera tool
   or a host-side Tuya-LAN client can replace the missing ONVIF PTZ. This is the recommended route (section 5).
3. No change to `enable_local.sh` can fix ONVIF PTZ, so none is attempted. Only a read-only diagnostic was added to
   its `verify` step (section 6).

## 2. Why `GetProfiles` has no PTZConfiguration (HIGH)

ONVIF server start path: `onvif_server_start` (0x232008, called from `yy_onvif_start` 0x231e1c, which is called
from the manager thread `yy_onvif_manager_th` 0x22fe38) -> `onvif_init_cfg` (0x239ec4) -> `onvif_init_cfg_default`
(0x23925c) then `onvif_init_cfg_by_param` (0x239b98).

- `onvif_init_cfg_by_param` ends with: `if (param->flag_at_offset_900 != 0) onvif_init_ptz_by_param(...)`
  (test at 0x239e24, call at 0x239e9c). Then it walks every profile (list head at global+0xb14) and stores the PTZ
  node list head (global+0xb18) into profile+20, which is the "PTZ configuration" pointer of the profile.
- `onvif_init_ptz_by_param` (0x239a98) is the only caller of `onvif_add_ptz_node` (0x2388e8) besides the dead
  twin `onvif_init_ptz` (0x2399b4, no caller anywhere). `onvif_add_ptz_node` creates the node and builds the
  tokens from the format strings `NODE_00%d` and `PTZ_00%d` (a shared counter, so the first ones are most
  likely `NODE_000` / `PTZ_000`; LOW on the exact digit).
- The parameter block (984 bytes, memset to 0 by the manager thread before each start) is filled by the static
  function at 0x231154. That function sets the enable word, names, addresses, stream count and one callback at
  +952 (it points to `yy_onvif_manager_get_local_ip`, 0x230ea4). A scan of the whole text section for any store
  with offset 900 finds no writer; the only reader is 0x239e24. So the flag stays 0 and no PTZ node exists.
- With no node, `profile+20` is 0. `onvif_PTZ_Stop` (0x24356c) and `onvif_ContinuousMove` (0x243218) test that field
  and return the code behind the SOAP fault string `PTZ Profile Not Exist` (`ter:NoPTZProfile`). This matches the
  hardware result of palier 5 exactly.
- `onvif_AddPTZConfiguration` (0x24ada8, called from 0x23f7d8, the `soap_AddPTZConfiguration` path) looks the
  configuration up with `onvif_find_ptz_cfg` (0x2388a0) in the same empty node list, so a client cannot attach one
  either.
- Callbacks: `onvif_init_cfg` copies parameter words +952..+984 into the ONVIF global at +64..+92. The PTZ entry
  that `onvif_PTZ_Stop` calls is global+72, which comes from parameter +960, never set (zero), so the stop and
  move handlers would be silent no-ops even with a node (MEDIUM: only the stop handler was traced to the
  callback; the move handlers share the same pattern).
- `ds_ptz_control` (0x242ba8) is two instructions (`mov r0,#0; bx lr`). The sibling stub `ds_get_ptzmaxspeed`
  (0x242b7c) only writes the constant 64. The string `todo : add stop PTZ moving code ...` also sits in the
  ONVIF PTZ block (evidence that the vendor never finished it, MEDIUM).
- Parameter names read by `yy_onvif_manager` (strings, HIGH): `[onvif]` section with `onvif_enable`, `onvif_on_off`,
  `onvif_ip_display`, `onvif_username`, `onvif_password`, `onvif_server_port`, `onvif_multicast_ip`,
  `onvif_password_sup_special_char`. Nothing PTZ related. The `[ptz]` keys read by `yy_ptz_init` (0x269754) are
  motion calibration only: `ptz_pan_step_max`, `ptz_tilt_step_max`, `ptz_pan_step_home`, `ptz_tilt_step_home`,
  `ptz_tilt_step_secret`, `ptz_manual_pan_speed`, `ptz_manual_tilt_speed`, `ptz_check_pan_speed`,
  `ptz_check_tilt_speed`, `ptz_check_pan_direction`, `ptz_check_tilt_direction`, `ptz_manual_pan_inverse`,
  `ptz_manual_tilt_inverse`, `ptz_track_*`, `ptz_secret_pos_enable`, `ptz_link_points`, `ptz_patrol_*`. None
  enables ONVIF.
- `onvif_on_off` and `rtsp_on_off` are the keys that already work (confirmed on hardware). A hidden
  "ptz on/off" key was searched for in the string table (`ptz_*`, `*_ptz`, `support`, `cap`): none exists.

Conclusion: do not try more `param.conf` keys. Writing speculative keys to the jffs2 CONFIG partition would only add
risk. Making ONVIF PTZ appear would need a binary patch of `yy_cam` (out of scope: appfs is squashfs and a patch
would be a vendor-binary derivative) or a proxy that speaks ONVIF PTZ itself (section 5, option C).

## 3. Native PTZ control inside the stock app

Both the cloud (Tuya DP) path and the local code use the same ladder (HIGH on the names and addresses):

- `yy_cloud_tuya_dp_response_ptz_control` (0x817e4): accepts a DP of type 3 (enum; byte at +1 of the DP record is
  compared with 3), reads the value at +4 and calls `yy_ptz_set_move_onoff` (0x26a080) with (2, 1, &value).
- `yy_cloud_tuya_dp_response_ptz_stop` (0x818f0): the stop DP.
- `yy_ptz_move_th` (0x2694d4) is the motion thread (named `Y_PTZ_MOVE_TH`); it calls `yy_ptz_motor_move`
  (0x270b28) and `yy_ptz_motor_stop` (0x270eac). Soft limits are enforced in `yy_ptz_check_move2lim` (0x26a918).
- The DP numbers (119 = direction 0..7, 116 = stop, per the third-party notes in PLAN section 2) are not visible in
  the code: they live in the cloud product schema. UNVERIFIED. The enum value is passed through to the motor
  layer unchanged (section 4), which supports "direction 0..7" (MEDIUM).

## 4. `/dev/tymotor` ioctl interface (MEDIUM-HIGH)

Opened once with `open("/dev/tymotor", O_RDWR)` in `yy_ptz_motor_init` (0x27077c, file descriptor kept in a
global). The string `/dev/ms41929` also exists in the binary, but the only `open` site found in the PTZ code is the
`/dev/tymotor` one (the ms41929 chip is written by the kernel module itself: symbol `ty_chip_ms41929_write`). The
module is `ty_driver.ko` (`ty_motor_ioctl` at .text+0x1c50, `ty_motor_open`, `ty_motor_release`; GPIOs
`motor_st1..4_gpio`, optional opto sensors).

Linux ioctl encoding, magic byte 0x0c:

| Name in `yy_cam` | Request (hex) | Decoded | Argument |
|---|---|---|---|
| `yy_ptz_motor_stop` | 0x00000c00 | `_IO(0x0c, 0)` | none |
| `yy_ptz_motor_get_pos` | 0x801c0c01 | `_IOR(0x0c, 1, 28 bytes)` | filled by the driver; the code zeroes an 8-byte buffer first and reads pan then tilt (twice, one call per axis) |
| `yy_ptz_motor_move` | 0x401c0c02 | `_IOW(0x0c, 2, 28 bytes)` | struct below |
| `yy_ptz_motor_move_to` | 0x401c0c02 | same request, command word 10 | absolute move |
| `yy_ptz_motor_param_init` (1) | 0x40100c03 | `_IOW(0x0c, 3, 16 bytes)` | calibration/limits (content not decoded) |
| `yy_ptz_motor_param_init` (2) | 0x40280c04 | `_IOW(0x0c, 4, 40 bytes)` | calibration/limits (content not decoded) |

Move argument (28 bytes, little endian, zeroed first): `word0 = command`, `word1 = a`, `word2 = b`, the rest 0.
`yy_ptz_motor_move(direction, a, b)` maps direction 0..7 to command 0..7 one to one through a jump table (any other
direction is rejected with an error log). Command 13 is the initial value used when nothing matches (not
reachable for valid directions). `move_to` uses command 10. The motion thread passes `a` and `b` taken from two
fields of the PTZ state (very likely the pan and tilt speeds from `ptz_manual_pan_speed` and
`ptz_manual_tilt_speed`; LOW). The direction numbering (which of 0..7 is left/right/up/down/diagonals) is NOT
decoded here: the hardware test must find it empirically, one step at a time.

Verdict: simple enough for a tiny static on-camera tool (open, one `ioctl`, optional stop ioctl, a few lines of C)
once the direction table and the meaning of `a`/`b` are measured. Open question: whether the driver allows a
second opener while `yy_cam` holds the node, and whether `yy_cam`'s position bookkeeping (`ptz_*_step_home`,
`yy_ptz_motor_get_pos`) gets out of sync if another process moves the motor. Home/limit calibration is done by
`yy_cam` at boot (`ptz check start!`), so a tool must not run during that phase.

## 5. Options to get PTZ locally, ranked

A. **On-camera tiny tool on `/dev/tymotor`** (needs a static ARM binary on the card; ioctls above). Fast, no
   cloud, no secret. Risk: concurrency with `yy_cam` (see above). Pair it with a small TCP/stdin front end on the
   card or call it through the SSH session started by `start_ssh.sh`.
B. **Tuya LAN DP client on the host** (DP 119 direction, DP 116 stop, UNVERIFIED numbers). Needs the device local
   key. Mechanism only (no value read or printed here): `yy_cam` keeps its Tuya activation data in the
   `/config/tuya/` directory, in the files `tuya_user.db` (plus a `tuya_user.db_bak` copy) and `tuya_enckey.db`; the
   SDK functions `tuya_ipc_get_local_key` and `tuya_svc_lan_decrypt_data` show the local key is held by the
   SDK and the LAN protocol is the standard Tuya encrypted one on TCP 6668 (open on the camera out of the box).
   Whether those databases hold the key in clear or protected by the `tuya_enckey.db` material is NOT known
   (LOW); the usual route is to obtain the key once from the owner's Tuya cloud account (device list). The
   key must never be committed or logged. Best for a host-side service (Phase 2), not for the card.
C. **ONVIF PTZ façade in the Akam assistant**: serve PTZ ONVIF towards NVR/Home Assistant and translate
   `ContinuousMove`/`Stop` to A or B. The camera's own ONVIF keeps providing video and events.

Recommendation: A for the card (zero secret, local), C on top of it in Phase 2, B as the fallback if A conflicts
with `yy_cam`.

## 6. Code change

`overlay/akam/enable_local.sh` `verify` (background, read-only) now also logs, with tests:
- the `[ptz]` values of `param.conf` (calibration numbers only),
- which pids hold a file whose name ends in `tymotor` or `ms41929` open (from `/proc/<pid>/fd` link names, no device
  path is built or opened),
- the motor entries of `/proc/devices`.
It changes nothing and keeps the static scan green. No new `param.conf` write was added: no key exists to set.

## 7. HARDWARE TEST PROCEDURE (coordinator)

Prerequisites: camera booted from the Akam card, SSH session available (`start_ssh.sh`), `yy_cam` running,
palier 5 state. Every step below is read-only unless stated. Nothing writes the config partition. Rollback of
every step is "do nothing / reboot with the card"; the one write-capable step (T6) uses a temporary file in /tmp.

T1. Confirm the card's new diagnostics (reboot not needed, run `enable_local.sh verify` by hand, or read the
    last lines of the boot log on the card):
    `sh /mnt/sdcard/akam/enable_local.sh verify; tail -n 12 /mnt/sdcard/akam/akam_boot.log`.
    Expected: a `param.conf [ptz]: ptz_pan_step_home=1275 ptz_tilt_step_home=816` line, `motor node tymotor open by
    pid: <yy_cam pid>` (this confirms the file descriptor claim of section 4), a `proc/devices motor entries:` line.
    If `tymotor` is held by nobody: the claim in section 4 is wrong; stop and report.

T2. Device nodes and module (read-only):
    `ls -l /dev/tymotor /dev/ms41929; cat /proc/devices | grep -i -E 'motor|ms4'; lsmod | grep -i ty_`.
    Expected: `/dev/tymotor` present as a character device (the major matches `/proc/devices`), `ty_driver` loaded.
    `/dev/ms41929` may be absent; that is fine.

T3. Re-confirm the ONVIF finding on the live camera (read-only SOAP on the camera's own address, using the
    existing probe): `GetNodes` and `GetConfigurations` on the PTZ service. Expected: no PTZ node (empty or fault),
    consistent with section 2. If a node appears after all (for instance after a firmware update), the whole
    section 2 is outdated; report it.

T4. Is the stock app still the only motor user? `ls -l /proc/*/fd 2>/dev/null | grep -E 'tymotor|ms41929'`.
    Expected: only `yy_cam` lines.

T5. Direction table, using the stock app itself (no new code). If a Tuya/Smart Life client can still reach the
    camera through the cloud or LAN, move it with the app's arrows and observe the physical motion. If not
    possible, skip to T6.

T6. Motion test with `akam_ptz` (built: `tools/build/bootstrap.sh && firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/build_akam_ptz.sh`,
    then `build_card.sh` copies it to the card as `akam/akam_ptz`, with `akam/akam_ptz.BUILDINFO`). It opens only
    `/dev/tymotor`, issues only stop / get position / move, caps a move at 1500 ms and always stops afterwards (also
    on SIGINT, SIGTERM, SIGHUP). Exit codes: 0 ok, 1 device or ioctl error, 2 usage, 3 EBUSY (another process holds
    the node; the message says so). Premise from T2/T4 (live camera): `/dev/tymotor` is misc 10,57 and only `yy_cam`
    holds it; whether a second opener is accepted is unknown: that is the first thing T6 learns.
    Start after the stock app finished its first calibration (wait about two minutes after boot, no motor noise).
    A person watches the camera and keeps a hand on the power cable. Every command below is one invocation.
    ```
    cp /mnt/sdcard/akam/akam_ptz /tmp/akam_ptz && chmod 755 /tmp/akam_ptz
    sha256sum /tmp/akam_ptz; cat /mnt/sdcard/akam/akam_ptz.BUILDINFO    # sha256_stripped must match
    /tmp/akam_ptz pos            # 1. read only: prints w0..w6 (expected: values near 1275 / 816 = ptz_*_step_home)
    /tmp/akam_ptz stop           # 2. no motion expected, rc 0
    /tmp/akam_ptz pos            # 3. same values as step 1
    ```
    Decision after step 1: `rc=3` (EBUSY) or any open error: stop, report the exact message. `pos` fails with an
    ioctl error: report errno, do not try `move`.
    Direction mapping (only if `pos` worked), one at a time, a/b left at 0 first, run `pos` after each:
    ```
    /tmp/akam_ptz move 0 300 ; /tmp/akam_ptz pos
    /tmp/akam_ptz move 1 300 ; /tmp/akam_ptz pos
    ... up to direction 7
    ```
    For each direction record: what moved (pan left/right, tilt up/down, diagonal, nothing) and the delta of
    `w0..w6`. If NOTHING moves with a=b=0 for any direction, retry the same direction with speeds, for example
    `/tmp/akam_ptz move 0 300 1 1`, then `move 0 300 10 10`; increase only while nothing moves, never above 100
    without reporting first (the meaning of `a`/`b` is a guess). Undo each move by the opposite direction with the
    same duration, or leave it: `yy_cam` re-homes at the next boot.
    Also check `dmesg | tail` after the series for driver errors (`init gpio error`) and that `yy_cam` is still
    the same pid. Abort criteria: any driver error in `dmesg`, motion that continues after the command returned
    (run `/tmp/akam_ptz stop`; if it keeps going, cut the power), `yy_cam` restarting, or the stock app starting a
    recalibration right after our command (note it, it means position bookkeeping is shared).
    Expected (UNVERIFIED): directions 0..3 single axis, 4..7 diagonals or the second family of moves.
    Rollback: remove `/tmp/akam_ptz` (it lives in RAM); reboot with the card re-homes the motor.

T7. Record the outcome in PLAN section 7 (palier 6): direction table, meaning of `a`/`b`, whether a second opener
    is accepted, and the position drift (if any) seen by the stock app afterwards.

Do not do on hardware (not worth the risk): adding speculative keys to `param.conf`, patching `yy_cam`,
flashing, or reading the Tuya databases.

## 8. Open items

- `akam_ptz` exists (`firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/`); its request numbers and the argument layout are
  still UNVERIFIED until T6 runs.
- DONE (see "Motor init without yy_cam"): the 16-byte and 40-byte `param_init` structures are decoded; tilt is
  readable with selector 1 (the "no tilt read-back" remark of T6 is superseded).
- Decode the direction numbering and the meaning of `a`/`b` (T6) and the effect of `ptz_manual_*_inverse`.
- Confirm DP numbers 116/119 and the local-key retrieval for option B (host side, Phase 2).
- Find the `/dev/ms41929` open site (probably a calibration or test path); not needed for the plan.

## End-stop safety (decided 2026-11-14, mandatory before any HA/ONVIF exposure)

The motor is a stepper with no limit switch: at a mechanical stop it skips steps (stock firmware does this on purpose
during its boot calibration). Whether software limits are enforced by `ty_driver` or only by `yy_cam` is unknown, and
`akam_ptz` talks to the driver directly. Therefore:

- T6 uses short moves (300 ms), default/low speeds, `pos` before and after each move, the user watching; stop a
  direction at the first unchanged position or grinding noise.
- Once the `pos` words are decoded, `akam_ptz` MUST gain (1) software limits from measured bounds (refuse moves past
  them) and (2) stall detection (position unchanged during a move → stop), before it is wired to Home Assistant/ONVIF.

## T6 results 2026-11-14 (hardware, akam-rot-01, `akam_ptz` sha256 21c8fa54…, a=b=0)

- Second opener OK: `/dev/tymotor` accepts `akam_ptz` while `yy_cam` holds it (no EBUSY). `stop` OK.
- `pos` word w1 = **pan position in steps** (home 1275 = `ptz_pan_step_home`). Increases to the right as seen from the
  front. All other words stayed 0: **tilt position is not reported** by this request (w2=0 although tilt moved).
- Pan speed with a=b=0: ~1030 steps/s (≈ +309 steps per 300 ms). No grinding heard during the test.
- Observed directions (user facing the camera): 0 = up; 1, 2, 3 = right (pan +); 4 = down; 5, 6, 7 = left (pan −).
  Matches the Tuya PTZ enum (0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left) for the
  pan component; no vertical component was seen on the diagonals (driver may move them pan-only, or tilt too small).
- Ended at pan 1279 (≈ home). Pan range/limits still unmeasured (max reached 2193); tilt range unknown and not readable
  → stall/limit protection for tilt cannot rely on `pos`; keep tilt moves short until a tilt read-back is found.

## Motor init without yy_cam (offline RE 2026-11-15, `yy_cam` + `ty_driver.ko`)

Context: in our-app mode (stock app never started this boot) `akam_ptz pos` reads 0 and `move 0 400` / `move 6 500`
return 0 but nothing visibly moves. Method: disassembly of `yy_ptz_init` (0x269754), `yy_ptz_motor_param_init`
(0x2709d0), `yy_ptz_motor_get_pos` (0x270fa0), `_move_to` (0x270d68), `_check_pan/_tilt` (0x2711b0/0x2712ec), the
`[ptz]` reader (0x26a1e4) and, in the module, `ty_motor_ioctl`, `set_init_ptz_cmd`, `set_init_ptz_cmd_else`,
`get_ptz_state`, `set_ptz_cmd`, `ms_timer_func`, `ty_motor_open` and the `g_motor_init` template. Only symbol names,
addresses and field offsets are recorded here. Nothing was run on the camera.

### What yy_cam sends at start (order)

`yy_ptz_init` -> `yy_ptz_motor_init` (only `open("/dev/tymotor", O_RDWR)`, with a fallback open of a second path) ->
`yy_ptz_motor_param_init(pan_max, tilt_max, pan_home, tilt_home)`, whose body is exactly:

1. `ioctl(fd, 0x40100c03, s16)` with `s16 = {0, pan_max, tilt_max, 0}` (zeroed 16 bytes, words 1 and 2 filled), then `sleep(1)`.
2. `ioctl(fd, 0x40280c04, s40)` with `s40 = {pan_home, tilt_home, 0 x8}` (zeroed 40 bytes, words 0 and 1 filled), then `sleep(1)`.
3. `yy_ptz_motor_get_pos` for both axes, result ignored by `param_init`.

The return values of both ioctls are ignored by `yy_cam`. Confidence: HIGH (read in the instructions).

Where the four values come from (`[ptz]` of `param.conf`, read by the function at 0x26a1e4 through `yy_conf_get_int`;
the homes are then clamped to 0..max):

| Value | key | code default | this unit |
|---|---|---|---|
| pan_max | `ptz_pan_step_max` | 4000 | no such key seen in param.conf: 4000 (MEDIUM, check on the card) |
| tilt_max | `ptz_tilt_step_max` | 2160 | same: 2160 (MEDIUM) |
| pan_home | `ptz_pan_step_home` | 2000 | 1275 (param.conf) |
| tilt_home | `ptz_tilt_step_home` | 1080 | 816 (param.conf) |

Other `[ptz]` keys and defaults (HIGH): `ptz_manual_pan_speed` 2, `ptz_manual_tilt_speed` 2, `ptz_check_pan_speed` 1,
`ptz_check_tilt_speed` 1, `ptz_check_pan_direction` 0, `ptz_check_tilt_direction` 1, `ptz_manual_pan_inverse` 1,
`ptz_manual_tilt_inverse` 0, `ptz_tilt_step_secret` 0, `ptz_secret_pos_enable` 1, `ptz_track_*`, `ptz_patrol_*`.
No GPIO/PWM pin, phase table or direction inversion is sent to the driver: they are fixed inside `ty_driver.ko`
(4 `motor_st1..4_gpio` lines and the chip-specific write routines selected by a type byte of the built-in template;
the optional opto-sensor GPIOs exist in the module, `yy_cam` never configures them). HIGH.

### Struct semantics (driver side, HIGH unless noted)

`ty_motor_ioctl` dispatches `0x0c00` stop, `0x401c0c02` -> `set_ptz_cmd`, `0x40100c03` -> `set_init_ptz_cmd`,
`0x40280c04` -> `set_init_ptz_cmd_else`, `0x801c0c01` -> `get_ptz_state`, under one mutex, with
`copy_from_user`/`copy_to_user` and an access check each.

- `0x0c03` (16 B): w0 -> `pan_roll_step` (dev+276), w1 -> `pan_max_step` (dev+280), w2 -> `tilt_max_step` (dev+284),
  w3 -> `infi_rot_en` (dev+308). **A zero word means "keep the current value"**. The module prints them with the
  format `now, pan_max_step=%d, tilt_max_step=%d, pan_roll_step=%d, infi_rot_en=%d`, which fixes the field names.
- `0x0c04` (40 B): w0 -> current pan step counter (dev+60), w1 -> current tilt step counter (dev+64); zero keeps
  the current counter; printed as `now, pls_pan_step=%d, pls_tilt_step=%d`. This request tells the driver "the camera
  is HERE now". Words 2..9 are read and ignored.
- The built-in template `g_motor_init` already holds limits 6000 (pan) / 2200 (tilt), speeds 5 (pan) / 4 (tilt) and
  counters 0/0. So without `0x0c03` the max is not 0; the real handicap is the counters (below).
- `get_ptz_state` (`0x801c0c01`) copies only 8 bytes in and out: **w0 is a selector, the answer is in w1**.
  Selector 0 = pan counter, 1 = tilt counter, 4 = (pan counter == 0), 5 = (tilt counter == 0), 7/8 = two internal
  flag bytes (dev+58/+59, meaning unknown, LOW), 9 = clear those flags, others return 0. `yy_cam` reads tilt with
  selector 1: **tilt IS readable**. T6 `akam_ptz pos` always sent selector 0, hence "no tilt read-back".
- `set_ptz_cmd` (`0x401c0c02`, 28 B, w0 = command 0..12): w1 > 0 -> pan speed (dev+296), w2 > 0 -> tilt speed
  (dev+300); zero keeps the previous speed (this is why `a=b=0` in T6 ran at ~1030 steps/s, the stock app's last
  speed 1, whereas a cold driver uses its slow 5/4 default). **Speed is a divisor: one step every `speed` ticks of a
  timer started with period 1000** (HIGH on the divisor, MEDIUM on 1 kHz): pan 1 = ~1000 steps/s, default 5 =
  ~200 steps/s, tilt default 4 = ~250 steps/s.
  - commands 0..7 (the "move" family): set the target counters to an extreme and let the timer walk the counter
    there: pan right = target `pan_max`, pan left = target 0, tilt up = target `tilt_max`, tilt down = target 0.
    0 up, 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left (matches T6 for pan; the
    diagonals move both axes). **The motor stops by itself when the counter reaches the target.** "Left" with the
    pan counter at 0 therefore does nothing, and neither does "down" with tilt 0.
  - 8 = stop pan, 11 = stop tilt, `0x0c00` = stop both.
  - 10 = move_to: w3 = pan target, w4 = tilt target, **clamped by the driver to [0, pan_max] and [0, tilt_max]**;
    w1/w2 = speeds. These are the driver's own software limits (they bound commands 0..7 too, via the counters).
  - 9 = check pan (calibration sweep): w1 = speed, w5 = direction. 12 = check tilt: w2 = speed, w6 = direction.
- No stall detection and no limit switch in the stepping code: the driver is open loop.

### Why our app saw no motion (MEDIUM-HIGH)

After a cold boot without `yy_cam` the counters are 0/0. `move 6` (left) targets 0 = already there: no motion by
design. `move 0` (up) targets 2200 at the default 4 ticks/step (~250 steps/s): 400 ms = ~100 steps, a few degrees of
tilt, easy to miss, and `pos` read pan only (still 0). So there is probably no hidden init needed for the motor to
run, only an unknown "where am I". Cheapest test, before any init: `akam_ptz move 2 500 1 1` (right, fast) must
raise `pan=` by ~500.

### Calibration / self-check in the stock app (HIGH on the mechanism, MEDIUM on the call order)

Mechanism: counting, not sensing. `check pan` (command 9), direction 0, sets the pan counter to `pan_max` and the
target to 0: the motor walks a full range toward the left, so whatever the start position it ends against the
mechanical stop (extra steps are skipped by the stepper) and the counter equals 0 there. Direction 1 sets the counter
to 0 and the target to `pan_max` (full walk to the other side, counter = `pan_max` at that stop). `check tilt`
(command 12) is the same on the tilt axis. `yy_cam` runs this once per boot in a state machine (`ptz check start!`):
`check_pan(ptz_check_pan_speed = 1, ptz_check_pan_direction = 0)`, polls the pan counter until it is <= 0 (direction
0) or >= `pan_max` (direction 1), then the same for tilt with speed 1 and direction 1 (tilt sweeps UP to `tilt_max`
by default), then goes home with `move_to(home_pan, home_tilt, speed, speed)`. A third mode of the check state (2)
re-measures the range; not decoded. `yy_cam` also writes the current counters back into `param.conf`
(`ptz_pan_step_home` / `ptz_tilt_step_home`, function at 0x26aaac) when the home position is stored, so those values
are "the position saved by the app", not a mechanical constant.

### Confidence summary

| Fact | Confidence |
|---|---|
| Init = `0x0c03 {0,pan_max,tilt_max,0}`, 1 s, `0x0c04 {pan,tilt,0...}`, 1 s | HIGH |
| `0x0c04` sets the driver's idea of the CURRENT position, not a target | HIGH |
| Counters are not sensed: after `0x0c04` the camera must really be there, or the counters lie | HIGH |
| 0x0c03/0x0c04 are all the init the module takes from userspace (no GPIO/phase data) | HIGH |
| Stock max values 4000 / 2160 when param.conf has no `ptz_*_step_max` | MEDIUM (code defaults; this unit's param.conf max keys not re-checked) |
| Our-app "no motion" = zero counters + slow default speed, not a missing init | MEDIUM-HIGH |
| Tilt readable with selector 1 | HIGH |
| Speed = ticks per step on a ~1 kHz timer | HIGH divisor / MEDIUM tick rate |
| Mechanical range in steps (pan, tilt) | LOW (4000 / 2160 never measured; T6 reached pan 2193) |

### Code change: `akam_ptz init`

(Superseded in part by "Safe motion layer" / "Shifted frame" below: `init --i-know [pan tilt]` now sends the SHIFTED
values, not 4000/2160; the replayed sequence and the allowed request tokens are unchanged.)

`akam_ptz init --i-know [pan_max tilt_max pan_home tilt_home]` (defaults 4000 2160 1275 816) replays steps 1 to 3
above and prints `pan=.. tilt=..`. It refuses without `--i-know`, refuses 0, values above the driver's own built-in
limits (6000 / 2200) and `home > max`, and never sends a move, a stop or move_to. `pos` now reads both axes
(`pan=.. tilt=..`, selectors 0 and 1). The C anti-brick scan allows exactly the two tokens `0x40100c03u` and
`0x40280c04u`, in `ptz/akam_ptz.c` only (positive and negative controls in `tests/rot2k_overlay_test.sh`).
Do NOT run `init` while `yy_cam` runs: it would overwrite its counters.

### `cal` plan (documented, NOT implemented, decided not to implement: see "Safe motion layer")

Needed only when the camera position after boot is unknown. It reuses the driver's own sweep but bounds the damage:
1. `init` with the stock limits, then `pos`. If the camera visibly sits at home, `cal` can be skipped.
2. Pan: command 9 {speed >= 3 (about 330 steps/s), direction 0}. The driver walks `pan_max` steps to the left stop
   (4000 steps = ~12 s at speed 3) and the counter becomes 0 there. Poll `pos` every 200 ms; send `0x0c00` and report
   if the counter does not decrease (driver idle) or after 1.5 x pan_max x speed ms. Do not use speed 1 on the first
   sweep of a new unit.
3. Tilt: command 12 {speed >= 3, direction 0 (down) first}, polled the same way. Then move_to home at speed 2..3.
4. Grinding at the stop is inherent to this design (the stock app does it every boot): one direction per axis, no
   repeats, explicit `--i-know`, and never launched by a script on its own.
5. Measure the real range once manually (sweep to the left stop, then move right with `pos` polling until the picture
   stops moving) and use the measured `pan_max` / `tilt_max` instead of the code defaults.
6. Commands 9 and 12 travel in the same `0x401c0c02` request as `move`, so the allow-list lives in the command words
   (enforced by `ptz_parse`/unit test), not in the request-number scan.

### Hardware procedure (coordinator, user watching)

Prereq: `yy_cam` NOT running this boot (our-app mode), card with the new `akam_ptz`, SSH session, a hand on the power
cable. First look: `grep -A12 '^\[ptz\]' /config/param.conf`; if `ptz_pan_step_max` / `ptz_tilt_step_max` exist, pass
their values to `init`.

```
cp /mnt/sdcard/akam/akam_ptz /tmp/akam_ptz && chmod 755 /tmp/akam_ptz
sha256sum /tmp/akam_ptz; cat /mnt/sdcard/akam/akam_ptz.BUILDINFO      # sha256_stripped must match
/tmp/akam_ptz pos                        # A. before init: expect pan=0 tilt=0
/tmp/akam_ptz move 2 500 1 1 ; /tmp/akam_ptz pos    # B. tests the "no init needed" theory: pan should rise ~500
/tmp/akam_ptz init --i-know              # C. takes ~2 s; expect pan=1275 tilt=816
/tmp/akam_ptz pos                        # D. same values
/tmp/akam_ptz move 2 300 1 1 ; /tmp/akam_ptz pos    # E. pan right, about +300
/tmp/akam_ptz move 6 300 1 1 ; /tmp/akam_ptz pos    # F. back left, pan near 1275
/tmp/akam_ptz move 0 300 1 1 ; /tmp/akam_ptz pos    # G. tilt up, tilt rises
/tmp/akam_ptz move 4 300 1 1 ; /tmp/akam_ptz pos    # H. back down
dmesg | tail -n 20                       # expect the driver line with pan_max_step=4000, tilt_max_step=2160
```

If B moved the camera, the camera is then away from the saved home by up to ~500 steps and `init` only declares that
spot to be home: acceptable for this test, reboot with the card afterwards (the stock calibration re-homes).
Abort criteria: grinding or clicking noise, motion that does not stop after the command returned (`/tmp/akam_ptz
stop`, else cut the power), a `pos` value that changes without motion or jumps, any driver error in `dmesg`.
Record in PLAN section 7: whether B moved, values after C, whether tilt moved on G, steps per 300 ms at speed 1.

## Hardware result 2026-11-15 — motor driven by our code without yy_cam (PASS)

Camera in our-app mode (yy_cam never started this boot), user watching each step: `pos` → pan=0 tilt=0 (cold
counters, confirmed); `init --i-know` (defaults 4000/2160/1275/816) → pan=1275 tilt=816, **no motion, no noise**;
`move 2 300 3 3` → pan 1375 (seen moving right); `move 6 300 3 3` → 1275; `move 2 600 2 2` → 1575 (clearly seen);
`move 6 600 2 2` → 1275 (back in front); `move 0 300 3 3` → tilt 917 (moved up); `move 4 300 3 3` → tilt 815. No
grinding, no driver error in dmesg. Speeds: divisor 3 ≈ 333 steps/s, 2 ≈ 500 steps/s (consistent with "1 step per
`speed` ms"). Directions confirmed: 0 up (tilt +), 2 right (pan +), 4 down, 6 left. Real mechanical ranges still
unmeasured (do NOT approach 0/max until the end-stop-safe `cal` plan exists).

## Safe motion layer (2026-11-15, `akam_ptz` + `ptz/akam_ptz_safe.h`; host-tested, NOT yet run on hardware)

Goal: make the motor safe to expose (Home Assistant / ONVIF later) without ever relying on the mechanical stops. The
motor has no limit switch and the driver cannot see the stops, so everything below is software, and the only truly
trustworthy numbers are the ones measured on the camera (procedure at the end).

### What changed

- `move`, new `goto`, `park`, `restore` go through one module (`ptz/akam_ptz_safe.h`, header-only, reusable by the app:
  fill a `struct ptz_ops` and a `struct ptzs_conf`, call `ptzs_move` / `ptzs_goto` / `ptzs_park`).
- **Soft range** (defaults pan 300..3700 of 4000, tilt 200..1960 of 2160). Justification: 4000/2160 are the stock code
  defaults, never measured (confidence LOW, table above), and the driver's counters only equal the mechanics if the
  camera was at home when `init` ran. The mechanical range is therefore probably NOT exactly 0..max; a margin of at
  least 200 steps (about 0.4 s at 500 steps/s, roughly 7-9 % of the range) on each side absorbs a few percent of
  error and the polling overshoot, and the stock home 1275/816 sits well inside. If the real range is smaller than
  that by more than the margin, only a measurement protects: see the procedure.
- **Move** (`move <dir> <ms> [a b]`, 1500 ms cap kept): refused (exit 4) when an axis it drives is within about one
  poll of its soft limit (`50/speed + 3` steps); otherwise polled every 50 ms and **clipped** (stopped, exit 0) when the
  next poll could cross the limit. Diagonals stop both axes at the first limit. Outside the range (for instance after
  narrowing `ptz.conf`) only travel toward the range is accepted.
- **Goto** (`goto <pan> <tilt> [speed]`): a target outside the soft range is **refused**, not clipped. Pan first, then
  tilt; each axis by timed single-axis moves (commands 2/6 and 0/4, hardware verified) with the wait before each poll
  shortened to exactly the time of the remaining steps, so it stops within a step or two of the target; up to 5
  correction passes (sleep granularity, overshoot), tolerance 3 steps, accepted up to 10, else exit 5. The driver's
  absolute move (command 10) is deliberately NOT used: although the driver clamps it to [0, max] (section above), that
  clamp is the counter range, not the soft range, and its behaviour is unverified on the hardware.
- **Stall / sanity**: a driven axis whose counter does not advance for more than 300 ms, a counter moving the wrong way,
  a failed `pos` read or a goto that exceeds twice its expected time all stop the motor and exit 5 (1 for I/O errors).
- **Speed is always explicit**: 2..20 divisors (default 3 from `ptz.conf`), `0` ("keep the previous speed") and `1`
  (~1000 steps/s, the stock app's sweep speed) are refused, because the driver may still be at the stock app's speed.
- **Uninitialised driver**: raw counters reading 0/0 refuse every motion (exit 4) and say what to run.
- **The driver's own limits are the soft range** (shifted frame, next section): a tool killed mid-move cannot be
  walked past a soft limit by the driver.
- The stop request is always sent last (3 tries), the position is printed and saved after every motion.
- No new ioctl request number is needed: the layer uses the three known ones (stop, pos, move) and sends only commands
  0..7 (the unit test's simulated driver counts any other command word as a failure).

### `akam/ptz.conf` (on the card, optional, strict `key=value`, `#` comments)

`pan_min pan_max tilt_min tilt_max` (soft range), `pan_home tilt_home` (park target, default `init` position),
`pan_step_max tilt_step_max` (driver limits sent by `init`, default 4000 / 2160, at most 4000 / 2160 = the stock numbering), `speed` (2..20,
default 3). Validation (any violation = exit 6, the motor is never touched; `stop` and `pos` still work): no unknown or
repeated key, whole-number values only; `min >= 50`; `max <= step_max - 50`; `pan_max <= 3700`, `tilt_max <= 2100` (was 1800; raised 2026-11-15 after the top stop measure: still 50 below `tilt_step_max` 2160) (measured stops: tilt top >= 2050 in the worst case, pan right >= 3932); range at least 100 wide; home inside the
range. An absent file means the defaults; an unreadable one fails closed. `akam_ptz limits` prints the effective
values. The tool never writes this file (no card writes, nothing to corrupt).

`pan_step_max` / `tilt_step_max` are no longer sent to the driver: they only name the stock numbering the soft range must
stay inside (the 50 step margins above). What the driver is told comes from the soft range (next section).

### Shifted frame: the driver's limits ARE the soft range (decided 2026-11-15)

Problem: commands 0..7 walk the driver counter to 0 or `step_max` and stop there, and the driver cannot know where the
mechanical stops are. A tool killed by `SIGKILL` (or a power glitch of the SoC only) during a move leaves the driver
walking to counter 0 / `step_max`, i.e. possibly into the mechanical stop. Fix: tell the driver a coordinate frame
in which ITS limits are our soft limits (no new driver request, same two init requests as the stock app):

    driver counter = real - off,   off = soft_min - 1
    0x0c03 {0, soft_max - soft_min + 1  (pan), same for tilt, 0}      (driver step_max = real soft_max)
    0x0c04 {home - off (pan), home - off (tilt), 0...}               (the CURRENT counters)

With the defaults: pan off 299, step_max 3401, home counter 976; tilt off 199, step_max 1761, home counter 617.
Counter 0 is real `soft_min - 1` (one step outside the soft range, 1 step = 2 ms, far inside the 200 step margin).

Validity against the documented driver semantics (all HIGH rows of the table above):
- `0x0c03` / `0x0c04`: a zero word means "keep the current value". Every value we send is >= 1 (step_max >= 101,
  counters >= 1: that is the reason for the `- 1` in `off`), so nothing is silently kept from an earlier init or from the
  driver's built-in 6000 / 2200.
- Commands 0..7 set the target counter to an extreme (`step_max` for right/up, 0 for left/down) and "the motor stops by
  itself when the counter reaches the target": with the frame above that extreme is the soft limit, so the stop is
  the driver's. The driver does not know it is "shifted"; it only counts.
- The order stays the stock one (limits, 1 s, counters, 1 s). A re-init with a smaller max while the old counter is larger is
  harmless: the motor is idle, and the counters are declared right after.
- Residual assumption (UNVERIFIED, tested on the hardware by the SIGKILL step of the procedure): the driver reads
  `pan_max_step` / `tilt_max_step` when a move command arrives and compares the counter for equality or
  "reached or beyond". Counters are never above `step_max` after init, so both readings behave the same.

Consequences, all implemented and covered by the unit test:
- The user-facing numbers (`pos`, `goto`, `move` clipping, `ptz.conf`, `pos` file) stay REAL. `pos` prints the real position;
  when the raw counters read 0/0 (uninitialised) or there is no frame it prints the raw counters (and says so on stderr).
- Because the mapping depends on the soft range at init time, the frame (`pan_min pan_max tilt_min tilt_max`) is kept in
  `/tmp/akam_ptz.frame` (same boot as the driver counters). It is invalidated before `init` talks to the driver and written
  only after the counters were read back and matched. No frame, a corrupt frame, or a `ptz.conf` whose soft range differs
  from it: `move`/`goto`/`park` are refused (exit 4) and nothing reaches the driver. A counter above the frame's `step_max`
  (someone else re-initialised the driver) is an error, not a position.
- Changing the soft range later: `pos` (still prints real coordinates, using the stored frame), then
  `init --i-know <pan> <tilt>` with those values: the camera has not moved, only the driver's frame changes.
- `init --i-know [pan tilt]`: `pan tilt` is the REAL position the camera is at now (default: `pan_home tilt_home` of `ptz.conf`);
  refused (exit 4) outside the soft range. The old form `init --i-know pan_max tilt_max pan_home tilt_home` is gone
  (the limits derive from the soft range).
- A raw 0/0 pair stays "uninitialised": a legitimate position never reads 0/0 because counters are >= 1 ... except a
  diagonal move cut by SIGKILL that the driver finishes at the very corner (both axes at counter 0): that also reads 0/0 and
  is refused; `restore` then refuses too (see below), so use `init --i-know <pan> <tilt>` with the real corner position.

### Persistence and reboot

- `/tmp/akam_ptz.pos` (`pan=<n> tilt=<n>`) is rewritten after every motion and after `init`. It survives app restarts
  within a boot and is the source for `akam_ptz restore` (driver counters back to 0/0 while the camera did not move,
  e.g. the motor module was reloaded). The driver counters stay the authority whenever they are not 0/0.
- Before every move (each correction pass of a goto too) the file is rewritten as `unknown` (fail closed: if that
  write fails the move is refused, nothing is sent) and replaced by the real position when the motion ends. A tool killed
  mid-move therefore leaves `unknown`, and `restore` refuses it, because the driver kept moving after the last known position.
  `restore` additionally needs the frame file and a `ptz.conf` equal to it, and the raw counters exactly 0/0.
- It lives in `/tmp`, so it is gone after a reboot, and the driver counters are 0/0 again. `init --i-know` then
  *declares* home (1275/816), which is only true if the camera was physically left at home. Hence **`akam_ptz park`
  before any clean shutdown or reboot** (the app/rc scripts that stop things should call it; nothing is wired
  automatically yet).
- Residual risk, not removable in software: a power cut, crash or `kill -9` while the camera is away from home leaves
  the next boot's counters wrong by that offset until a calibration. After such an event the soft range no longer
  matches the mechanics: look at the camera, and either bring it by hand-sized `move`s to the home view and `init`, or
  accept that the margins (200+ steps) are the only protection.
- A move cut by `SIGKILL` is no longer a residual risk: the driver keeps walking, but toward counter 0 / `step_max`, which
  are the soft limits (shifted frame). The camera ends at a soft limit, the position file says `unknown`, and `pos` still
  reads the true position from the driver (the counters survive; only the tool died). SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGPIPE
  are caught and stop the motor first. What remains: counters that are wrong because the camera was not where `init` was told.

### `cal`: not implemented, on purpose

`akam_ptz cal` is refused with an explanation. The documented plan above (driver commands 9 / 12) cannot be made
safe, for three reasons: (1) the point of the sweep is to *end against the mechanical stop* (the driver walks a full
`max` from a wrong counter), so it always grinds, which is the thing to avoid; (2) the counters are open loop: a
stalled stepper still counts, so software can neither detect that the stop was reached nor measure the range, and the
"abort if more than 10 % beyond the expected range" criterion can never fire (the driver stops itself after exactly
`max` steps); (3) one-ended sweeps only reference one end, the far end stays unmeasured. What can be done safely is a
**manual, watched measurement** with the soft limits widened step by step (procedure step 8), which is optional.

### Tests

`tests/rot2k_ptz_unit.c` drives the real code against a simulated driver with counters, speeds, a mechanical position
with hard stops lying inside 0..max, stall / reversed / lost-stop / read-failure injection and clock jitter. It checks
soft-limit refusal and clipping on every direction and both axes, diagonals, goto convergence (also with jitter, from
one step outside the range, slow motor), stall detection timing (stopped within 300..400 ms), park, persistence and
`restore` (including corrupt, `unknown`, frame-less and frame-mismatch cases), `ptz.conf` validation, the shifted frame
(init values, frame file lifecycle, failed inits leave no valid frame, `pos` in real coordinates), and that over a long
mixed session the simulated mechanics never hit a stop and no driver command other than 0..7 is sent. The kill test
kills the tool (longjmp, no stop request) in 1536 move cases (8 directions x 8 starts including all four corners x 8 kill
times x 3 speeds) and in goto/park cases, then lets the simulated driver run free for 40 s with the hard stops placed ONE
step outside the soft range: the mechanism must never reach a stop. A control case with the stock unshifted limits does
grind, so the test is able to fail. `tests/rot2k_overlay_test.sh` keeps the anti-brick C scan: the
init request tokens only in `ptz/akam_ptz.c` (negative control on `akam_ptz_safe.h`), no other `/dev` path, no 9/10/12
command word in the move path.

### Hardware procedure (user at the camera, hand on the power cable, one step at a time; stop at the first doubt)

Prereq: our-app mode (`yy_cam` not running), card built with the new `akam_ptz`, SSH session, camera sitting at its
normal front/home view. Run each step, report the output, wait for the user's "go" before the next one.

1. `cp /mnt/sdcard/akam/akam_ptz /tmp/ && chmod 755 /tmp/akam_ptz`; compare `sha256sum` with
   `akam/akam_ptz.BUILDINFO`. `/tmp/akam_ptz limits` (expect pan=300..3700 tilt=200..1960 home=1275/816). No motion.
2. `pos` (expect the raw 0/0 after a cold boot, with a note on stderr), then `move 2 100 3 3`: must be **refused,
   exit 4, nothing moves**.
3. User confirms the camera is at home by eye. `init --i-know` (default home 1275/816): it prints pan=1275 tilt=816, no
   motion; `pos` prints the same REAL values (the driver counters are now 976/617 and its limits 3401/1761: `dmesg | tail`
   shows `pan_max_step=3401, tilt_max_step=1761` and `pls_pan_step=976, pls_tilt_step=617`); `cat /tmp/akam_ptz.pos`
   and `cat /tmp/akam_ptz.frame` show the real position and `pan_min=300 pan_max=3700 tilt_min=200 tilt_max=1960`.
4. Four short moves, one at a time with the user watching, `pos` after each: `move 2 300 3 3` (pan ~1375, right),
   `move 6 300 3 3` (back ~1275), `move 0 300 3 3` (tilt up ~917), `move 4 300 3 3` (back ~816).
5. Clip test in a **narrow** range far from any stop: write `akam/ptz.conf` with `pan_min=1100`, `pan_max=1450`,
   `tilt_min=700`, `tilt_max=950`, `pan_home=1275`, `tilt_home=816`; `limits` shows it. `move 2 100 3 3` is now refused
   (exit 4, "ptz.conf soft range ... differs"); run `pos` (real, still the old frame), then `init --i-know 1275 816` (the
   camera is there; the driver now has limits 351/251 and counters 176/117). Then `move 2 1500 3 3`: must
   stop on its own with "clipped", pan <= 1450 (~175 steps, ~0.6 s of motion, not 1500 ms); `move 6 1500 3 3` stops at
   >= 1100; the same for tilt up/down. Also try `goto 1500 816` and `goto 1275 960`: both refused (exit 4).
5b. **Driver-limit test, the point of the shifted frame** (still in the narrow range, camera at 1275/816, user
   watching): `/tmp/akam_ptz move 2 1500 3 3 & p=$!; sleep 0.3; kill -9 $p; sleep 3; /tmp/akam_ptz pos`. The tool is
   killed during the move, the camera must keep turning right for another ~0.3 s and stop on its own, `pos` must read
   pan=1450 (the soft maximum) and the tilt unchanged, never beyond 1450; `cat /tmp/akam_ptz.pos` says `unknown`. Then
   `goto 1275 816`, and the same with `move 6` (stops at pan=1099 or 1100), `move 0` (tilt 950) and `move 4` (tilt 699 or 700)
   and a diagonal `move 1`. If the camera does NOT stop by itself at the soft limit, the driver does not behave as
   documented: stop, `stop`, report, and rely on the tool's polling only (do not use the SIGKILL-safety claim).
6. `goto 1350 880`, `goto 1200 760`, `goto 1275 816` at the default speed; the printed position must be within 3 steps
   each time and the camera must visibly return to home. Repeat `park` three times from different spots. Optionally
   `goto 1350 880 2` (fastest allowed speed).
7. Stall detection cannot be provoked without touching a stop, so it is verified in simulation only; do not try it.
8. OPTIONAL, only if the user wants the real range, and with the explicit decision that finding an end means touching
   it once, gently: widen one edge at a time in `ptz.conf` (e.g. `pan_min=200`, then 100, then 50) in about 100-200 step
   increments (after each edit: `pos`, then `init --i-know <pan> <tilt>` with the printed position), `goto <edge> 816 10` (divisor 10 = ~100 steps/s, ten times slower than the stock boot sweep), user
   looking and listening at the camera, hand on the cable. Stop at the first moment the picture stops changing or a
   click is heard, run `pos`, and set that axis' soft limit at least 200 steps inside the last position where the
   camera visibly still moved. Record the measured values here and in `akam/ptz.conf`; do NOT widen beyond what was seen.
9. Park test: `park`, `pos`, then reboot with the card; before running `init` check by eye that the camera is at home.
   After a restore test (optional): `rmmod`/reload of the motor module is NOT part of this procedure; `restore` is only
   covered by the simulation.
10. Record in PLAN section 7: step results, positions, any noise, the measured ranges (if step 8 was done).

## Hardware result 2026-11-15 (card-less) - safe motion layer PASS

move before init refused (exit 4); init --i-know 1275/816 without motion; move right/left/up/down 300 ms at speed 3 gave pan 1385/1276 and tilt 922/815, seen by the user, no grinding. Narrow-range kill test (pan 1100..1450): tool killed right after starting a long move to the right, driver stopped by itself at pan=1450 (soft max), pos file unknown; goto 1275 816 returned to 1272/815. Default range restored. BusyBox sleep takes integers only.

## Mechanical range measured 2026-11-15 (user listening for the stop, 200-500 step moves at speed 3)

Pan: left stop = 0, right stop between 3932 and 4151 (total about 4000, the stock default). Tilt: bottom stop = 0, top stop between about 2050 and 2480 (stock default 2160). The level front view (set by hand) is pan about 1275, tilt about 1300 above the bottom stop (the stock home 816 looks lower). The old default tilt soft max 1960 was ABOVE the real top stop in the old frame. New soft range, at least 300 steps from the worst-case stop: pan 300..3650, tilt 300..1750, home 1275/1300. DONE (see "hal_ptz" below): these are the code defaults of `akam_ptz_safe.h` and of the app descriptor; the app keeps its files in /config/akam (the card-less /mnt/sdcard is tmpfs).

## hal_ptz: the motor in `akam2k_rtsp` (host-tested with a fake driver, NOT yet run on hardware)

Files (`platforms/anyka-ak3918av130/`): `hal_ptz.h/.c` (portable engine: calibration, saved position, goto / step /
nudge / cancel on top of `ptz/akam_ptz_safe.h`, reused unchanged except the new defaults, `ptzs_conf_parse_over` and a `PTZS_QUIET` switch),
`hal_ptz_drv.c` (Linux driver: node, ioctl, atomic files), the 2K descriptor in `hal.c` (`/dev/tymotor`, command words 0 up, 2 right,
4 down, 6 left, the measured range above, park 1275/300 (down; was 1275/1750), tilt soft max 2100 (was 1750; head level-up at about 2104, top stop 2290..2340), speed 3, button step 200), `ptz.h/.c` (one thread, one pending command).
Unit test: `tests/rot2k_hal_ptz_unit.c` (simulated motor with hard stops; part of `tests/rot2k_overlay_test.sh`).

**Files on the camera** (`/config/akam`, jffs2): `ptz.conf` optional, same strict keys as the tool (`pan_min pan_max tilt_min tilt_max pan_home
tilt_home pan_step_max tilt_step_max speed`); a bad one switches PTZ off (motor never touched); the app never writes it. `ptz.scene` (+ `.a0..a2`) is the
HOME reference (see "No recurring flash writes" below). There is NO `ptz.pos` any more (2026-11-16): the position lives in RAM; a leftover file
from an older version is ignored. (The `akam_ptz` debug tool still has its own `/config/akam/ptz.pos` / `PTZ_SAVED_POS_PATH` writes; it is not part
of normal operation.)

**Boot** (`ptz_cal=auto`, default): (0) a stop request, then (1) the driver already holds our frame file (`/tmp/akam_ptz_app.frame`, written
next to `/tmp/akam_ptz.frame`, RAM) and non-0/0 counters (the app restarted in the same boot): adopted, no init; (2) otherwise, i.e. EVERY COLD BOOT,
the scene check may prove that the head is at HOME (the only claim a cold boot can make: no position is stored anywhere, so there is no stale file to
believe), else the calibration, then state home. The head can move outside the app (by hand, `akam_ptz`, a stock firmware boot, a power cut while it
was away from home) and the open-loop counters cannot detect it (safety review, MEDIUM; the stock calibrates at every boot as well). The adoption is
refused (so the next start calibrates) after a stall or a failed calibration (the app invalidates both frame files), and after any `akam_ptz`
motion / `init` / `restore` (the tool invalidates `/tmp/akam_ptz_app.frame`). `ptz_cal=off`: the motor is not initialised, the state
stays `unknown`, every motion is refused (still adopts case 1, which moves nothing).

**Calibration method chosen: declare-max-then-move** with the verified directional commands, the same mechanism as the stock `check` commands
(counter set to the step maximum, motor walked toward 0 until the driver stops at 0). It needs no 9 / 12 command word, so the anti-brick rule
"commands 0..7 only" and its scan stay as they are. Sequence: init the stock frame (limits and counters 4000 / 2160, i.e. "I am at the far
end"); tilt DOWN (command 4) then pan LEFT (6) at speed 3, polled every 50 ms until the counter reads 0 (the driver stops itself there;
stall / wrong direction / 1.5x timeout abort); both axes now sit on the stops = real 0/0; init the "approach" frame (real 1/1, driver limits
home + 20) and `goto` home with the safe layer (so even a killed process cannot overshoot home); init the final shifted frame at home. About
12 s + 6.5 s of sweep, 2 x 3 inits (1 s each) and 4 s of approach, roughly 40 s, at every cold boot, never on an app restart in the same boot. Grinding against the
stop for the rest of the counted steps is inherent (the stock does it at speed 1 every boot); the sweep length is the stock `step_max`
(4000 / 2160, from `ptz.conf`), enough from any position up to the soft range.

**State** (from the position): `home` (within 15 steps of home), `parked` (within 15 of park; tested first), `moved`, `calibrating`, `unknown`
(not initialised, a failed calibration, or after a stall; the next boot calibrates). `park` = pan home, tilt soft max (looking up); `ptz_park=pan:tilt`
(clamped into the soft range). `ptz_privacy=1`: the RTSP clients get no video frames while `parked` (sessions stay up; on leaving it the client
resumes at the next keyframe, requested at once). Audio is not touched.

**API** (`ptz.h`): `a2k_ptz_toggle_park()` (parked -> home, else park), `a2k_ptz_nudge(dx, dy)` (relative steps, summed while queued,
never cancels), `a2k_ptz_command(A2K_PTZ_*)`, `a2k_ptz_info()`, `a2k_ptz_privacy_active()` (one volatile read on the video path). Motor
commands run on the PTZ thread only; one motion at a time; a new home / park / step cancels the running motion (stopped within ~50 ms, position
kept) and replaces the pending command; every command is refused while calibrating.

**akam2k.conf keys** (passed by `own_app_run.sh`): `ptz_cal` auto|off, `ptz_park` PAN:TILT (default 1275:300), `ptz_privacy` 0|1, `ptz_scene_check` 0|1, `ptz_scene_min` 0.80..0.99, `track_invert_pan` / `track_invert_tilt` 0|1.

**MQTT / Home Assistant** (same filtering as the LED controls: retained commands ignored, 20 commands per 10 s, strict payload): buttons
`ptz_left` `ptz_right` `ptz_up` `ptz_down` (one step of 200 steps), `ptz_home`, `ptz_park`, `ptz_recal` ("Recalibrate PTZ": the long calibration now; refused with `ptz_cal=off`) (command topic `BASE/<id>/set`, payload exactly `PRESS`);
Continuous motion by MQTT (not a Home Assistant entity, no discovery): the command topic `BASE/ptz_vel/set`, payload `PAN TILT [LEASE_MS]` (a space or a comma between the numbers; PAN and TILT -1..1 with up to 4 decimals, LEASE_MS whole 150..2000, default 450) or `STOP`;
the same mapping, lease, soft limits and priority as the ONVIF ContinuousMove (see [docs/onvif.md](../../onvif.md)); retained messages are ignored; own budget of 5 per second (the general 20 per 10 s of the other commands could not hold a move); a refusal (parked, calibrating) is silent, `ptz_state` shows why.
sensor `ptz_state` (enum home / parked / moved / calibrating / unknown); diagnostic sensors `ptz_pan`, `ptz_tilt` (steps, only while known).

**Hardware test plan (user at the camera, a hand on the power cable)**: (1) with `ptz_cal=off` boot: log says "not initialised", HA state
`unknown`, buttons do nothing. (2) `ptz_cal=auto`, no `/config/akam/ptz.scene`: watch the calibration (tilt down, pan left, then home); abort criteria:
grinding louder than the stock boot, a motion that does not stop, the log line `calibration FAILED`. (3) after it: buttons in all four directions
(about 200 steps), park (looks up), home; `/state` `sys.flash_writes_ptzpos` stays 0. (4) reboot with the head at home: the home view matches, no calibration (or it calibrates once and writes the reference); reboot with the head moved away or turned by hand: it calibrates. (4b) `akam_ptz move 2 200` while the app is stopped, then restart the app: it calibrates (no adoption). (5) `kill -9`
the app during a motion: the next start finds no adoptable frame and calibrates (or proves home); `kill -TERM` restarts the app: adopted, no calibration. (6) `ptz_privacy=1`:
park, the RTSP client freezes, home, the video resumes within one GOP.

## Cold-boot calibration and cancel race (safety review follow-up)

- Cold boot calibrates unless the HOME scene reference proves the head is at home; no position is ever read from the flash; same-boot app restart adopts (own frame file + non-0/0 counters); a stall or a
  `akam_ptz` motion / init forbids the adoption (frame files invalidated). Host-tested (`tests/rot2k_hal_ptz_unit.c`, `tests/rot2k_ptz_unit.c`).
- `ptz.conf` caps: `pan_max <= 3700`, `tilt_max <= 2100` (was 1800; raised 2026-11-15 after the top stop measure: still 50 below `tilt_step_max` 2160), `pan_step_max <= 4000`, `tilt_step_max <= 2160` (the old 6000 / 2200 driver limits are
  no longer accepted).
- `hal_ptz_cancel` bumps a generation counter; the controller calls `hal_ptz_mark` when it dequeues a command, and `begin()` returns
  `HAL_PTZ_ECANCEL` when a cancel arrived between the dequeue and the start of the motion (it used to be cleared and lost).

## Tilt top stop measured 2026-11-15 (evening, 50-step moves, user listening)

In the app frame (bottom stop = 0 by calibration): the head is level/straight at about 2104 (no stop); the top stop is between 2290 and 2340 (about 2315). New tilt soft max proposed 2000 (>= 290 steps margin). Park changed to DOWN (1275:300) at the user request. Tracking pan direction is inverted on this unit (invert_pan=1).

## Scene check: skip the cold-boot calibration when the head provably did not move (host-tested, NOT yet run on hardware)

Problem (user, hardware): the long calibration at EVERY cold boot (sweeps of 4000 / 2160 steps) grinds about ten times against the stops on both axes.
The long calibration stays the default and the fallback; it is skipped only when the camera image proves the head is where it was left.

**Reference** (`/config/akam/ptz.scene`, < 4 KB, text header `scene1 <mode> <pan> <tilt> 80 48 <boots> <stamp>` + 3840 raw gray bytes; or `invalid [<boots>]`; the stamp is the write time, epoch seconds, 0 = unknown): the HOME position and an
80x48 grayscale thumbnail (box filter 4x4 of the Y plane of the 320x192 detection frame, taken at most every 500 ms by `a2k_scene_feed`, no extra capture),
with the day / night mode at capture and the number of matched boots since the last full calibration. Written in TWO steps on the PTZ thread, so that the
thumbnail and the position belong to the same moment (safety review M1): (1) after a calibration of this process, once the head is at home and the motor has been quiet for 6 s (the AE has
settled; and 10 min after the last tracking nudge) the current view is kept in RAM as the candidate (the capture runs without the controller lock); (2) 6 s later (or at the app shutdown, if the candidate is more than 3 s old) the PTZ thread
RE-READS the driver counters (they must still equal the cached position, within 4 steps) and the scene module writes the candidate only if the current
view still matches it (same ZNCC + block rule, same day / night mode). A view that changed, counters that differ, or no settled candidate: nothing is
written (up to 3 retries, then the next cold boot calibrates unless the old reference still matches). Atomic (`akam2k_store`); a view without detail is never saved (and not logged as saved).
Superseded 2026-11-16: the reference is no longer rewritten after every move: see "No recurring flash writes" below.

**Boot decision** (`app/scene_check.c`, pure, `tests/rot2k_scene_unit.c`, `tests/rot2k_scene_glue_unit.c`): BEFORE any motor command the PTZ thread waits
for the first frame + 4 s + 6 frames (AE settling; a day/night change restarts it; 30 s timeout), then compares (algorithm of 2026-11-16, see "Low-texture
views" below; the first version used a global ZNCC and an 8-of-12 block rule and refused a plastered white wall): the day/night mode must be the same,
at least 4 of the 12 blocks must be "detailed" in both images (clearly above the image noise), at least 2/3 of those must match on their own, the peak
of the displacement must lie within +-2 thumbnail px (about +-8 source px) and the mean score of the matching blocks must be >= `ptz_scene_min`
(default 0.90, lowest accepted 0.80). Match: HOME is declared to the driver (final shifted frame, no motion, no calibration; the reference position must equal the configured home, else
"not taken at home": calibrate), EXCEPT when home is the park position (within 15 steps; the parked view is the least informative one): that calibrates. Every 10th matched boot is a full calibration
anyway (L1: the counter `boots` lives in the reference and is rewritten at each match; if it cannot be written, no match). Anything else (no reference,
view too uniform, mode differs, mismatch, no frame, error, timeout, position outside the soft range) is the full calibration as before. The score and the
decision are logged (`ptz: scene check: MATCH, score 0.987 >= 0.900 (boot 3 of 10 ...)`). `ptz_scene_check=0` disables everything; without the
detection (`detect=0`) there is no frame source: always calibrates.

**Invalidation** (superseded 2026-11-16; it used to be "fail closed on every motion"): the reference is keyed to HOME, so moving the head away does not invalidate it
(a stale reference can only match when the live view IS the home view, i.e. the head is at home). It is overwritten with `invalid <boots>` only when (a) a calibration
FAILS (not a shutdown cancel), or (b) a position claimed by the scene check is lost (stall / I/O error: the claim was proven wrong). Best effort, counted, at most once per
run; a failing drop is logged and refuses nothing. `akam_ptz` (the debug tool) writes the marker on any motion / init (`PTZ_SCENE_PATH`).

**Calibration sweeps** (M3): the real tilt travel is about 2315 steps (top stop 2290..2340) but the declared step maximum is 2160, so one sweep could
end about 150 steps above the bottom stop. The calibration now sweeps tilt down, re-declares the tilt counter at 300 and sweeps down again (the pan
counter keeps its maximum), then pan left: four inits instead of three, the motor always ends on the bottom stop. Host test: head at the top stop.

**Limits (documented, tested)**: the correlation ignores a global gain / offset and a lighting change over part of the image (lights on / off, AE, a lamp: matches);
a covered lens, a dark night view or a wall without any relief above the noise prove nothing (rejected: calibrate, or the multi-view fallback); a very repetitive
pattern can match after a shift of one period; a displacement of 3 thumbnail px (about 12 source px) or more is refused; moved furniture or people covering up to a third of the
detailed blocks are tolerated, more forces a calibration (safe). The one thing that must never happen, a match on a view that proves nothing, is excluded by the detail
check on BOTH images (and in common), and a match carried by a fixed pattern by the vote of the other blocks.

### Low-texture views (2026-11-16, host-tested on the real wall frame, NOT yet run on hardware)

Problem (hardware, home 1275/1300): the home view is a close white plastered wall (window edge at the right, blue cloth bottom right). The reference was refused
at commit: "too few image blocks match on their own (a fixed pattern?), score 991". The global score was 0.991, but the old rule needed 8 of 12 blocks to have
detail (absolute std dev >= 8 and gradient >= 1.5) in BOTH images, and most blocks were low-contrast plaster. Requirement: the camera must adapt even when the
reference view is a white wall.

**Algorithm** (`scene_check.c`, thumbnail 80x48, grid 4x3 = 12 blocks of 20x16):

1. *Band image*: 49 x (sum of the 3x3 pixels) - 9 x (sum of the 7x7 pixels), a difference of boxes. Zero on a flat area and on a linear gradient: a global gain /
   offset, vignetting, a slowly varying illumination and the IR hot spot vanish; edges and plaster relief of a few thumbnail px stay.
2. *Noise floor from the image itself*: the 25th percentile of the Laplacian energy of the 8x6 sub-blocks (the quietest part of the picture is noise only; texture
   everywhere only OVERestimates it, which makes the check refuse more, never accept more), converted to the band image (x 882, white noise), with a floor of
   sigma 0.6 gray level (8-bit rounding, banding of a perfect ramp). A block is *detailed* when its band energy is >= 3 x that noise energy. A block without detail is
   EXCLUDED (neutral), never counted as a mismatch.
3. *Score*: each block detailed in both images is scored by the noise-compensated zero-mean normalised correlation (the noise variance of each image is
   subtracted from its variance, not from the covariance) over displacements of +-4 thumbnail px, on the part of the block where both images exist. The scores are
   SUMMED over the voting blocks: the displacement is the peak of that surface (one smooth block cannot tell a shift of a pixel or two, ten blocks can).
4. *Decision* (MATCH): same mode; >= 4 detailed blocks in the reference, in the current view and in common (`SC_DETAIL_BLOCKS_MIN`); the peak within +-2 px
   (`SC_TOL`, about 8 source px; one thumbnail px is about 4 source px) and better than every larger displacement by 0.02 (`SC_PEAK_MARGIN`, per block); >= 2/3 of the
   voting blocks with a score >= min - 0.10 at that displacement; and the mean of the matching ones >= min. Otherwise: no match (fail closed, full calibration).

**Fixed pattern / vignetting / IR hot spot (protection kept)**: a smooth camera-fixed pattern (vignetting, IR hot spot) correlates at every shift and at every
scene; the band image removes it, so it produces no detailed block: a view that only contains it is "too uniform". A localised high-contrast one (lens dirt, an
overlay) produces a few detailed blocks that match at zero shift whatever the scene; the other blocks of the scene (or of ANOTHER scene) vote it down (< 2/3), and
a head turned shifts the summed peak away from zero (or flattens it: the peak margin fails). Residual limit (accepted, documented): a high-contrast pattern fixed to
the camera that covers 4 or more of the 12 blocks of a completely featureless scene cannot be told from a real scene; day/night equality and the periodic full
calibration (every 10th matched boot) bound it.

**Thresholds** (tuned on the real frame, then checked with the synthetic scenes): detail ratio 3, `SC_DETAIL_BLOCKS_MIN` 4, 2/3 vote, tolerance 2 px, peak margin
0.02, block margin 0.10 below `ptz_scene_min`. Results on the real wall (`tests/fixtures/scene/wall_80x48.gray`, the 80x48 thumbnail of the frame, noise sigma 1.5 gray
levels): 8 to 11 of 12 blocks detailed (the old rule needed 8 blocks with absolute detail in both images and refused it); same view + noise: match 20/20; mild
exposure change (gain 0.85..1.15, offset -6..+6): match 20/20; a lamp lighting half of the wall: match; shifts of 1 px and 2 px: match; shifts of 3 px (4 directions) and
6 px: refused; mirrored wall, head turned by 10/4 px: refused; the wall without relief (plane through its plaster) + noise, a flat image, an IR-like vignette alone, a lens
dirt spot: "too uniform" (never a match); wall + vignette: match; wall + vignette with the head turned: refused. A Monte Carlo of 1800 flat / vignetted / ramp images with
noise (thumbnail sigma 0.3 to 2.8) gave 0 images with 4 or more detailed blocks and 0 false matches. Even the plaster-only crops of the frame (no window, no cloth) are
decided (4 to 7 detailed blocks); the sparsest crop has 3: undecidable by itself, which is what the multi-view fallback is for.

**Log wording**: "the saved view is too uniform to prove anything", "current view too uniform (too little detail in common with the saved view)" (no more "fixed pattern?").

**Multi-view fallback for a truly featureless rest view** (hal_ptz `cold_offset` / `cold_probe`, `ptz.c` `run_multi`, scene hooks `probe*` / `alt_*` /
`commit_multi`): when the rest view has fewer than 4 detailed blocks, `capture` answers 2 and, at HOME only (not parked, not an arbitrary tracking position), once
per run, the PTZ thread moves the head by up to three small offsets from the rest position (pan +150, pan -150, tilt -100 steps, each skipped when it would leave the
soft range), keeps the view of each one (quiet 6 s, kept, confirmed 6 s later: it must still match), returns to the rest position (counters re-read, they must still agree)
and writes ONE multi-view reference: the stored views `ptz.scene.a0..a2` first (same format `scene1`, position = rest + offset), the main `ptz.scene` with the
featureless rest view LAST (the stored views are only ever used with a valid main reference of the same position, mode and offsets). The old reference is not touched before the new one is complete; a command, a shutdown or any failed move abandons the build (nothing is written; the next cold boot calibrates). At a cold boot, a featureless saved view
with usable stored views answers "probe": the claimed position is DECLARED to the driver (no motion), the head moves by an offset with the safe-motion layer
(`ptzs_goto`, no new ioctl), waits 1.5 s and 3 new frames, compares the live view with the stored view of that offset with all the rules above; on a match it goes back to
the claimed position (trusted, boots + 1); otherwise it tries the next offset, and after the last (or any failed / cancelled move) the position is forgotten again and the
full calibration runs. Never at the park position.

**Review fixes (Fable, same day)**: (H1) the frame files (`/tmp/akam_ptz*.frame`) stay INVALID while the position is only claimed (the in-RAM frame drives `ptzs_goto`);
they are written once a stored view proved the position, so a hard kill during the 5-15 s probe cannot make a restart of the app adopt an unproven claim. (H2) the vote is
over the UNION: a block detailed on one side only and clearly flat on the other (< 1.5x its noise) counts AGAINST; a borderline one is ignored (exposure, flicker). A reference
of 8 scene blocks + 4 fixed-pattern blocks against the pattern alone is refused. (M1) a repetitive texture is never saved as a reference or as an offset view ("repetitive texture, no reference"); the 2026-11-16 residual fix is below. (M3) the multi-view build runs only when
the reference is fresh (after a calibration) or once per run, never within 10 min of a tracking nudge; only a real failure (no usable view, failed move, write error)
counts, 3 tries per run at most; an interruption (command, shutdown) is not a failure. (L1) the probe is requested only when the LIVE rest view is featureless too and
the day / night mode is unchanged. (L2) the head goes back to the claimed position between two offsets, so the travel from the real position is always one D. (L3) a
shutdown during the probe (`cold_probe` answers -1) returns cancelled: no calibration starts. (L4) `scene_matched` is cleared when the position is unknown or a calibration
ran. (L5) obsolete: the probe adds no flash write any more (no `ptz.pos`). The probe keeps the default speed (the configured divisor).

**Risk analysis of the probe move (D)**: at boot the position is only CLAIMED; the head may have been turned by hand anywhere. The probe is a relative move of at most D
steps (the driver limits are the soft range of the declared frame; `hal_ptz` also refuses any offset above 200 steps per axis or outside the soft range), so in the worst case the
head ends D steps beyond where it REALLY was. Soft range pan 300..3650 vs stops at 0 and 3932..4151 (measured): D_pan = 150 leaves 150 steps (left) and at least 282 - 150 = 132 steps
(right) to the stops even from the soft-range edge. Tilt 300..2100 vs stops at 0 and 2290..2340: the tilt offset is DOWN only (-100): from the soft-range edge 300 it ends at
200, 200 steps from the bottom stop; going up would leave only 190 - D (about 90) steps to the top stop, hence never. Host test with the simulated motor (hard stops at 0 and
4000 / 2300): from the corners (3650/300 and 300/300) with a wrong claim, the probe moves cause no click (the calibration, which grinds on purpose, only starts afterwards). A head
that really was at a stop (outside the soft range) could click at most once per offset: the same sound as the calibration, never a stall (the driver limits are the soft range).
Time cost: about 2 s per offset (move 0.5 s + 1.5 s settle + frames), at most three offsets, only when the rest view is featureless; the reference build moves the head once per
position (about 12 s of waiting per offset, at the first quiet moment at home after a calibration).

**Open-loop limit (accepted)**: the head is a powered open-loop stepper. Turning it by hand within the capture window after a motion (before the candidate is written) can pair a moved view with unchanged counters. It is bounded by the periodic full calibration every 10 matched boots; a restart of the app in the same boot (adoption) keeps the on-disk count (`invalid <boots>` marker), it does not restart it.

**Recalibrate button** (HA `ptz_recal`, `A2K_PTZ_RECAL`): the long calibration now from any state (the home reference stays, it is checked again when quiet at home;
only the matched-boots counter restarts, one write, and only when the counter was not 0 or the home view changed); every command is refused meanwhile; refused with `ptz_cal=off`; a cancel (shutdown) between the dequeue and the start is honoured.


## No recurring flash writes (2026-11-16, host-tested, NOT yet run on hardware)

User rule: no recurring writes to the flash in normal operation; state lives in RAM; a write only for an explicit user change or a rare one-off event, each counted
(`/state` `sys.flash_writes_ptzpos`, `sys.flash_writes_scene`, `sys.flash_writes_store`). The PTZ persistence was redesigned for it.

**Position: RAM only.** The safe-motion layer still reports through its `PTZ_POS_PATH` (`/tmp/akam_ptz.pos`); `hal_ptz` keeps the value in RAM and nothing reaches `/config`
(no `unknown` before a move, no position after a quiet period, no probe-path or `declare_final` write). The frame files `/tmp/akam_ptz*.frame` are RAM (`/tmp`), used only to adopt
the driver's frame after an app restart in the same boot. `sys.flash_writes_ptzpos` stays 0 (the key is kept for API stability). The old `ptz.pos` is ignored.

**Boot decision (cold boot)**: (1) the driver holds our frame (warm restart, same boot): adopt (RAM files only), no scene check, no write. (2) Otherwise the ONLY claim is HOME
(the configured `pan_home` / `tilt_home`), and it is proven by the home reference: the live view must match it (single view, or the multi-view probe around home when the home
view is featureless: stored offset views at pan +-150 / tilt -100). Proven: home is declared, no calibration, no motion. Not proven, no reference, a reference taken at another position
(home reconfigured), a repetitive reference, a day/night mode that differs, every 10th matched boot, no frame in 30 s, home == park: full calibration. There is never a claim from a stale
file. A power cut while the head was away from home (tracking left it there, `track_return=stay`) therefore costs one full calibration at the next boot: the only cost of the policy
(superseded: `home` is now the default, see tracking.md; `stay` keeps this exposure).

**The home reference** is keyed to home: it proves "the head is at home", whatever moved it since, so moves never invalidate it and nothing is dropped before a move or a calibration.
It is checked / written only after a calibration of THIS process (the head is at home then), once the head is quiet at home for 6 s and no tracking nudge happened for 10 min:
- no reference (or invalid, or taken at another position): written (stamp = now, boots 0);
- the stored one still matches the candidate: nothing is written (after a calibration only the matched-boots counter restarts, one write, and only when it was not 0);
- the home view changed: rewritten, at most once per 24 h; the rate limit lives in the file itself (the `<stamp>` of the header: no extra file, no extra write); an unset wall clock
  (no network) forbids a rewrite of an existing reference; an unknown stamp (older file) allows it;
- the other day/night mode than the reference: kept (no flip-flop; the cost is a calibration at a cold boot in the other mode);
- invalidated only by a failed calibration or a claimed position that proved wrong (stall / I/O error).
At most 4 reference writes per run (belt and braces). The matched-boots counter (10-boot forced recalibration) is written at most once per boot (the match itself; the file is
rewritten with boots + 1): there is no RAM-free scheme for a counter that must survive power cuts, and a cold boot is a rare event.
The travel budget stays RAM only (`ptz.travel_since_cal`; a cold boot after a power cut calibrates anyway when the head was away from home).

**Flash writes in normal use** (a person crossing every 2 min, 12 crossings per hour, `track_return=stay`, the former default): `ptz.pos` 0 (was 60 per hour = 1440 per day), scene reference 0 (was up to 12 per
hour), i.e. 0 per day, for any number of moves. Per cold boot: at most 1 write for the matched-boots counter when the view matches, or after a calibration at most 1 write (new reference, a changed
view once per 24 h, or the counter restart), plus 1 for a failed calibration or a lost claim. Host tests (`tests/rot2k_hal_ptz_unit.c`, `tests/rot2k_scene_glue_unit.c`): zero `ptz.pos` writes across 12 tracking
crossings, zero scene writes and drops across moves, cold boot claim = home proven by the reference, a power cut away from home calibrates, the rewrite limit of 24 h.

**Residual fixes of the scene re-review** (same day): (R1) a block STRONGLY detailed on one side (energy >= 2 x the detail threshold) counts against the match whatever the other side shows
(it was ignored in the borderline band 1.5..3 x noise). (R2) `sc_repetitive(thumb, min)` skips the blocks that fall off the image (no averaged -1000), searches every displacement (step 1,
+-40 px: periods up to 40 px), applies the SAME decision rule as the match (2/3 of the valid blocks >= min - 0.10, their mean >= min) lowered by a 0.05 margin (R3: a clean
self-correlation is an upper bound of a live noisy alias), and does not flag the broad main lobe of the correlation (the passing displacements connected to the tolerance box: vertical
edges, an invariance along one axis; the match refuses those by itself through `peak_ok`). References already on the flash are screened at the boot check (and the stored offset views at `load_alts`):
repetitive -> calibrate. Sweep on Fable's synthetic harness (square / sine bars, lattices, dots, periods 4..40, three scene mixes): 283 of 286 alias-match cases flagged (the 3 left are
dot lattices of period 37..40 px = more than half the image), 0 of 200 random smooth scenes flagged, 0 flags on the real frames and the wall fixture, vertical-edge scenes not flagged. (R5) the
screening runs outside the controller mutex, with integer sums (the host takes about 15 ms, an estimate of a few hundred ms on the ARM9). (R6) the multi-view probe snapshots `cancel_gen` at the start
and tests it before every move (a cancel eaten by a position poll or by `begin()` is no longer lost). (R7) the probe attempts are bounded per boot by a RAM marker (`/tmp/akam_ptz.probes`, 2 attempts, then
the calibration decides: a crash loop never walks the head by an offset at every restart). (R8) `long long` in the energy / noise arithmetic (a strong edge squares to more than 2^31).

**Review follow-ups (Fable, same day)**: (M1) the main-lobe exclusion of `sc_repetitive` is bounded: a lobe reaching beyond 8 px (`SC_REP_LOBE_MAX`) is flagged too (an alias chain looks like a broad lobe); the price is that a pure 1D invariance (every row equal) is flagged, which is conservative (the match refuses it anyway); sweep: 0 unflagged alias cases of the lobe harness, 3 of 286 left in the period harness (dots 37..40 px). (M2) the strongly-detailed one-sided vote compares with the LARGER noise of the two images (a noisy live view no longer turns a strong block into a vote against; the real wall with sigma 2 noise matches 38 of 40 again). (M3) a reference of the other day/night mode is a changed view: replaceable under the same 24 h rule in the single-view, the keep-flat and the multi-view paths. (L1) an unknown stamp (or a not-set clock) allows at most one rewrite per run, and the per-run write cap also guards the multi-view build. (L2) no reference is checked or written once more than 3000 steps were travelled since the calibration (`A2K_PTZ_REF_TRAVEL`: lost steps could pair a moved view with home). (L3) the reference is keyed to the configured home, not to the reported position. The `akam_ptz` debug tool no longer writes `ptz.pos` or invalidates `ptz.scene` (no flash write; it still invalidates the /tmp app frame). (L4) the boot check logs `timing: repetitive screening N ms, compare M ms` to measure on the camera (R5 estimate).

### Illumination-robust vote and the stored repetitive verdict (2026-11-16, host-tested, NOT yet run on hardware)

First hardware boot after the check shipped: reference written at dusk with a lamp on, cold boot a few minutes later, head not moved:
`timing: repetitive screening 3008 ms, compare 65 ms` then `too few of the detailed image blocks match on their own (score 0.962, needs 0.900)`.
Reproduced on the host from the stored `ptz.scene` and a stream-1 frame of the same view (fixtures `tests/fixtures/scene/ptz_dusk_lamp_ref.gray`,
`ptz_dusk_now.gray`, thumbnails only). All 12 blocks are detailed in both; at zero shift 5 blocks score 0.88..0.98, 5 plaster blocks (relief 3..7 x the
noise, flattened by the lamp) score 0.1..0.67 with no better shifted peak, and the two window / cloth blocks (energy 2.3x and 1.7x lower) peak at a 2 px
shift (0.93 / 0.98) while scoring 0.30 / 0.33 at zero. The old rule counted every block below 0.80 as a mismatch (5 of 12 matched, 2/3 needed).

New rule (`sc_compare`): the displacement is the peak of the summed surface (within `SC_TOL`, `SC_PEAK_MARGIN` above larger ones), then each block is
classified at it: SAME (peaks there, score >= min - 0.10), MOVED (a shifted peak >= min - 0.20 beats it by 0.12: against), mismatch (a strong block
peaking nowhere: against), neutral (weak blocks, lighting-lowered strong blocks that still peak at the common displacement, at most 2 blocks whose
energy changed by more than 2x without correlating). MATCH needs peak_ok, >= 4 SAME blocks that are >= 1/3 of the common detailed blocks and >= 2/3 of the voting
blocks (neutral blocks that are not weak weigh half a vote against), a mean SAME score >= min - 0.05, and the pooled weak blocks >= 0.25 (a weak block is SAME only at >= min - 0.10:
its noise-compensated correlation is unstable). A block that
correlates keeps its SAME vote whatever its energy did (a gain). A noise estimate above 2 x the floor (`NV_CREDIBLE`: texture inflates it) makes no block weak.
Fixed-pattern protection: the union vote is untouched; neutrality is limited to weak or changed blocks and they are pooled / half weighed, so a pattern
that covers a few blocks cannot win over another scene (tests: another textured scene and another plaster wall behind the same pattern: 0 accepted).
The repetitive gate is the same match rule applied to the image against itself (a weak block that passes counts as SAME, strong failures count against).
Residual risk (honest), measured with a synthetic harness (another wall of unrelated relief + a camera-fixed pattern of 4 to 6 blocks, 40 draws per
amplitude; old rule -> new rule accepted): relief amplitude 0.3 (below the detail threshold) 40 -> 32..39, 0.4: 16..27 -> 3..27, 0.5: 0 -> 1..12, 0.6 and above: 0 -> 0.
The hole was already there for a relief below the detail threshold (those blocks are not voting at all); the new rule moves its edge, it does not close it:
a pattern that is detailed over 4 to 6 blocks while the rest of the view is a wall with almost no relief can still carry the vote. Real fixed patterns
are smooth (lens dirt, hot spots: removed by the high-pass) or absent from the detection channel (no OSD there), which is why this was accepted.
Also: when most blocks are weak plaster, 4 SAME blocks are needed; a window that dims to half and clips takes the right column out of the evidence, with fewer than 4 SAME blocks left the check refuses (calibrates).

Performance: the repetitive verdict is computed once when the reference is captured and stored in the header (`scene2 ... <stamp> <rep> <repmin>`:
the threshold it was screened at; trusted only when repmin equals the configured threshold). The boot only reads it. An older `scene1` file, or one screened at another
threshold (also the offset views), is screened once at the boot (slower once), and the store of the matched boot writes the verdict. The screening itself got an
early exit and a cheap `dsqrt` (5 Newton steps from an exponent-halving seed instead of up to 60 soft-float divisions: the camera has no FPU and this
was most of the 3008 ms): 17 ms -> about 4 ms on the host, estimated 0.3-0.7 s on the camera (not measured). The coarse gate was dropped (it only
mattered at boot). Log: `timing: repetitive verdict read from the reference ...`.
`hook_drop` invalidates any version ("scene" + a digit). Downgrade: an older build does not read `scene2` (no reference: full calibration).
Review follow-ups (Fable): the jump ratio is normalised by the median block-energy ratio (a global gain / contrast no longer flips blocks: the real pair
matches from gain 0.8 to 1.5 and 100/100 with +0.5 noise); `SC_SAME_BELOW` 0.25 -> 0.10 and `SC_SOFT_BELOW` 0.50 -> 0.25 (a window 60 % brighter that
clips is now refused; 30 % is tested); `sc_unpack` refuses a screening threshold below 0.80; main-lobe seeds of the repetitive screening reach `SC_TOL +
SC_REP_BRIDGE` px (the home view + 0.5 noise was flagged 12 of 60 times by an isolated fringe of its broad vertical lobe, now 0 of 60; the
ceiling-line family of the review harness stays flagged and the unflagged alias counts are not above the old rule's, except layout 1 mix 0.80:
25 vs 16 of 124). A tried "SAME blocks must localise along each axis" rule was dropped: it reopened invariant-axis aliases.

### Ambiguous home view: multi-view instead of giving up (2026-11-16, host-tested, NOT yet run on hardware)

Hardware (lamp night): after the calibration `ptz: scene: repetitive texture, no reference` (the near-flat wall with vertical bands / frame edges is
invariant along y: flagged by the 1D rule) = no reference = full calibration at every cold boot. The stream-1 frame of that view (fixture
`ptz_home_night_lamp.gray`, thumb rebuilt like the others) is NOT flagged on the host (0 of 15 noise draws), so the flag on the camera could not be reproduced
from the frame; the mechanism is tested with a synthetic lattice. A repetitive / ambiguous home view now takes the multi-view path of the featureless one:
`capture` answers 2, the controller stores the views at the offsets (pan +-150, tilt -100: each must itself be detailed and not repetitive), the home view is
written LAST in the main file with its verdict (`rep 1`). At the boot a stored `rep 1` never decides: with usable offset views the answer is the probe
(the head moves by an offset with the safe-motion layer only, `sc_probe_check` compares there), else the old refusal at once. The probe safety is the
unchanged one. A plain commit refuses any featureless / ambiguous candidate. With a recent (< 24 h) reference on the flash the 24 h rule keeps it.

**Diagnosis dumps (always on, RAM only).** Every capture and every boot check / probe writes the exact thumbnails the app used to `/tmp/akam_scene_dbg/`
(80x48 raw gray, 3840 bytes; tmp + rename; the same names overwritten, at most 6 files, about 12 KB; never the flash; not behind a config key since the cost
is nil): `cap.gray` + `cap.txt` (the capture candidate: flat / ambiguous / usable, per-image noise, detailed / strong block masks, the repetitive verdict with
the displacement and lobe kind that triggered it), `ref.gray` / `live.gray` + `ref.txt` / `live.txt` (boot or probe: the reference and the live view, the
comparison counts, SAME mask, shift, score, verdict). The repetitive trigger is also logged (`repetitive screening triggered at displacement dx,dy`).
The boot does not screen again for the dump (it reads the stored verdict). Fetch with scp / `cat` and view with `ffmpeg -f rawvideo -pix_fmt gray -s 80x48`.

## 2026-11-15 hardware follow-ups

- Park default is now 1275:300 (down, 300 steps above the calibrated bottom stop). Tilt soft max 2100 (level-up view about 2104; top stop 2290..2340).
- Person tracking: the pan direction was inverted on the 2K (a person on one side turned the camera away, pan 1275 -> 996 -> 695). The 2K now defaults
  `invert_pan=1` (akam2k.conf `track_invert_pan`, `track_invert_tilt`; tilt default 0, unverified).

## 2026-11-16 tracking follow-ups

- Tracking nudges can ask for a speed divisor (`a2k_ptz_nudge_sp`, `hal_ptz_nudge_sp`): the same `ptzs_goto(speed)` of the safe-motion layer (2..20,
  anything else refused), no new ioctl. The tracker uses 2 (~500 steps/s; verified on 2026-11-15 for pan only, one run) for a large correction, the configured 3 (~333 steps/s)
  otherwise. A nudge is accepted only while the controller is idle.
- `a2k_ptz_info_t.motion_end_ms` (CLOCK_MONOTONIC ms, same clock as the detector) tells the tracker when the last motion ended, so it drops only the frames
  captured during the motion (+ 150 ms) instead of waiting a fixed time.
- A tracking nudge costs: 30 ms start, 1 step per `speed` ms, about 70 ms to stop, read and publish; the motor never runs two commands at once.
  Field numbers behind the tracking rework (person at 1-2 m, speed 3): see tracking.md, "the latency chain".

## 2026-11-17 stale reference after a hot update

Field log (cold boot after a hot update): `scene check: scene differs: calibrating`, then at home `the home view ... differs from the reference (score -1000), which was written less than 24 h ago ...: not rewritten`. The 24 h rewrite limit kept a reference that this very boot had just proven stale, so every cold boot compared against it and recalibrated until 24 h had passed. Also `repetitive verdict screened now (older reference file) 870 ms`: the screened verdict of an older reference file was only stored back on a match.

- When the boot check (or a probe) refuses the stored reference (mismatch, mismatch blocks, mode difference, repetitive, low detail of the live view) and a calibration follows, the post-calibration capture at home may replace the reference once in this run, whatever its age (`g_stale_boot` in `scene.c`). The other gates are unchanged (position verified by the calibration, travel gates, quiet period). Spent by the first successful write (single or multi-view); the same 24 h rule applies to every other case (a view change seen without a failed boot, a matching boot).
- Not a stale verdict: no reference, taken at another position, periodic full calibration, no frame, cancelled, store failed.
- The repetitive verdict screened at the boot (older file / another `ptz_scene_min`) is stored back right away (one write), so the 870 ms screening happens once, not at every boot.
- Tests: `tests/rot2k_scene_glue_unit.c` ("a reference this boot just refused may be replaced once").

## 2026-11-17 Continuous motion (host-tested on a fake driver, NOT yet run on hardware)

Measured with `akam_ptz` on 2026-11-16/17 (timed moves, speed 3): diagonal commands work (1 = pan+ tilt+, 3 = pan+ tilt-, 5 = pan- tilt-, 7 = pan- tilt+; both axes move at once, ~370 steps/s each, so the rate is about 1100 / divisor); a new direction command sent during a move is accepted at once WITHOUT a stop (pan reversed 1181 -> 1200 immediately). The tracker uses this: `ptz/akam_ptz_safe.h` `ptzs_cont_*`, `hal_ptz_cont_run` (`hal_ptz.c`), `a2k_ptz_vel` / `a2k_ptz_pos_at` (`ptz.c`), `track_motion=continuous` (tracking.md, "Targeting policy, person state and continuous motion").

- **Driver requests: none new.** Only the move (commands 0..7, diagonals included), the stop and the position read of the safe layer. The position file is marked unknown once per session (not per command).
- **One session = one ongoing move.** `hal_ptz_cont_run(first command, lease, callback)` runs on the motor thread; after every 50 ms poll the callback may return KEEP, a new / refreshed command, or STOP. Commands are logical (right / up signs, divisors per axis) and become command words through the descriptor (`dir[]`, `diag[]`; a descriptor without diagonals turns a diagonal into the faster axis alone).
- **Retarget without a stop** (`ptzs_cont_set`): a reversal or a change of direction / speed is just a new move request. Exception, on purpose: when an axis LEAVES the command (a diagonal becomes a pure direction, or the axis hit its limit) the stop request is sent first and the new move after it, because what the driver does with an axis that is missing from a new command is not measured (test: with a fake driver that keeps unlisted axes running the dropped axis does not run away).
- **Dead-man lease.** The motion stops by itself when no command (an identical refresh counts) arrived for the lease (150..2000 ms, default 450): once in polled time in the safe layer (lease + 150 ms), once in real time in the controller thread (`a2k_ptz_vel` stamps the mailbox). A tracker that stops calling (a blocked detection thread) stops the head within about half a second.
- **Soft range, continuously.** On every poll an axis whose room to the soft limit is under 1.5 polls of travel + 3 steps is dropped from the command (a diagonal continues as the pure direction of the other axis); with nothing left the motion ends (`HAL_PTZ_OK`). `ptzs_room` semantics as in `ptzs_move`; the driver's own limits are still the soft range (shifted frame), so a late poll cannot pass a limit. A command toward a limit the head is already at is refused (nothing is sent).
- **Stall / wrong way** exactly as in `ptzs_move` (300 ms without progress, counter going the wrong way by more than 2) per commanded axis, but the detector of an axis restarts at every command change on it and gives no verdict for 150 ms (a reversal needs a moment to show). A stall at a soft limit is a clip, not a fault. An axis that is not commanded and moves by more than 3 steps per poll after the grace is an error (stop sent, `HAL_PTZ_ESTALL`, position no longer trusted: the usual fail-closed handling).
- **Travel** is the sum of the polled steps (reversals included) and feeds `hal_ptz_travel` (the wear budget / scene reference limit); the session end waits 60 ms after the stop before the final position read (a stop takes a moment), so the position saved after it is the settled one.
- **Preemption.** Any command through `submit` (step, home, park, recalibrate, goto), a MANUAL nudge (API `ptz.move`), `a2k_ptz_stop` call `hal_ptz_cancel`: the session ends at the next poll (<= 50 ms) with `HAL_PTZ_ECANCEL`, the motor stopped, the position kept. The tracker's own nudge and velocity commands are refused (-1) while another command runs; a velocity command while a session runs only updates the mailbox. Privacy (parked), calibration, tracking disabled, the area rule and `track_motion=steps` stop the session from the tracker side (`a2k_ptz_vel(0, 0, 0, 0)`).
- **Position history.** The controller keeps the position of every poll (~3 s) and the position at the end of every motion; `a2k_ptz_pos_at(t)` interpolates it (resolves also while a session runs, "unknown" before the first sample, after a non-session motion in between or after a gap during a session). The tracker positions each frame with it (`capture time - lag_ms`).
- **Tests:** `tests/rot2k_ptz_cont_unit.c` (fake driver: 1100 / divisor steps/s, command latency, shifted frame + mechanical stops, injectable stall / wrong way / drift, "unlisted axes keep running"; lease, refresh without driver traffic, reversals every 100 ms, diagonals, dropped axis, both limits and the corner, stall, wrong way, uncommanded axis, cancel, bad speeds, no diagonals descriptor, then the controller thread: sessions, `a2k_ptz_pos_at`, the manual command / nudge / park / shutdown preemption, and zero forbidden driver requests). Part of `tests/rot2k_overlay_test.sh`.
- **To verify on the camera:** (1) a diagonal command that becomes a pure direction does not leave the other axis running (the layer stops first, but look at `moves although it is not commanded` in the log); (2) a speed change in the middle of a move is honoured (compare the polled rate with 1100 / divisor); (3) the lease: while a person is followed, hide the camera view (cover the lens: the detector keeps ticking with no person) or disable detection; the head must stop within about 0.6 s of the last refresh. The dead-man is software only: a hang of the whole process (not only of the detection thread) cannot stop the motor, the driver's own limits remain the only backstop.

### Continuous motion: review follow-ups (same day)

- **A dead process cannot stop the motor by itself.** The lease is software; a directional move is only bounded by the driver's own limits (the soft range), so a killed or crashed app could leave the head walking until the end of travel. Two complements: (a) the fatal-signal handler of the app (`on_crash`: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) first sends the motor stop request on the already open node (`hal_ptz_emergency_stop`: async-signal-safe, one `ioctl` stop sent twice, nothing else); (b) the supervisor (`own_app_run.sh`) polls the app every 1 s (was 5 s) and, as soon as the app is gone (crash, SIGKILL, or its own stop), runs `akam2k_rtsp --ptz-stop` (opens the node, one stop request, exits; no calibration, no thread, no other request). SIGKILL and a hard hang cannot run (a); (b) then stops the head within about 1 s. Tests: `rot2k_ptz_cont_unit.c` (emergency + one-shot stop send only the stop request), `rot2k_overlay_test.sh` (the supervisor asks for the stop after a crash and after its own stop signal, the stop call is not an app run, poll default 1 s).
- **Hardware kill tests still required (nothing here ran on the camera):** with a person-free scene and a hand on the power cable, start a continuous move and `kill -9` the app: (1) pure tilt up from a mid position, (2) a diagonal (up-right) in a narrow frame, (3) `kill -STOP` the app and wait. Expect the head to stop within ~1 s (supervisor), or, if the supervisor is also dead, to end at the driver limit (soft range), never at a mechanical stop. Record how far it travelled.
- Diagonals use divisor 3 or slower (both axes moving); divisor 2 is single-axis only (a diagonal at divisor 2 was never measured). Tracking areas: the velocity command is refused toward an area edge within `30 + rate * (tick 200 ms + lease)` steps (385 steps at divisor 2): the head cannot pass the edge before the next refresh or the lease can stop it, at the price of a wide unused band near an area edge at high speed. A session that is refused at a soft limit publishes and logs nothing; a velocity stop cancels a start that has not run; a command arriving while a session is ending is refused (or starts the next session).

## Hardware kill tests, 2026-11-17 (driver limits are the backstop for continuous motion)

Narrow frame written in RAM (/mnt/sdcard/akam/ptz.conf on the card-less tmpfs: pan 1100..1450, tilt 1150..1450, home 1275/1300),
`akam_ptz init --i-know 1275 1300`, user next to the camera, hand on the cable. Each time the tool was SIGKILLed 0.3 s into a
1500 ms move at speed 3 (no process left driving the motor):
- tilt up (`move 0`): killed at tilt 1406, the driver stopped by itself at **1450** (the frame maximum) within 0.4 s, no overshoot.
- diagonal up-right (`move 1`): killed at 1381/1406, **each axis stopped at its own limit, 1450/1450**.
- diagonal down-left (`move 5`): killed at 1170/1193, both counters stopped at **0/0 = the frame minimum (1100/1150)**. Note: the
  tool then prints "raw driver counters (no initialised position)" because 0/0 is also the value of an uninitialised driver —
  a display ambiguity of the shifted-frame design, not a motor problem (the app recalibrated cleanly afterwards).
Conclusion: the driver's own limits stop a dead process at the soft range on both axes and on diagonals, as documented for pan
right on 2026-11-15. This is the backstop the continuous mode relies on (plus the crash-handler stop and the supervisor stop).
Config removed and the camera recalibrated via the API after the test.

## 2026-11-17 calibration noise: fewer false refusals, short calibration (host-tested, NOT yet run on hardware)

Field log: `scene check: too few of the detailed image blocks match on their own (score 0.970, needs 0.900): calibrating` with a zero shift: the head had not moved, a person / furniture / the light changed some blocks and the strict 2/3 vote refused. Two changes, both fail closed for a moved head.

**(a) Displacement evidence (`sc_decide`): NEVER a proof (revised after the review).** The relaxed rule (exactly zero shift, no MOVED block, enough blocks matching on their own with a mean score >= the threshold) does not return SC_MATCH: it only makes the verdict SC_LIKELY ("very likely home"). SC_MATCH is exactly what it was before (the strict 2/3 + union vote), at boot AND for the proof after a short calibration, so the false-trust rate of the scene check is unchanged by construction. The field benefit is kept only through LIKELY -> short calibration -> strict proof (a person standing in the view during the proof may still need the full calibration). Test: `rot2k_scene_unit.c` `test_half_same` (3 / 4 replaced blocks: the strict vote proves it as before; 5 / 6: LIKELY, never MATCH; a 3 px shift: neither; another scene: refused).

**(b) Short calibration.** A new verdict `SC_LIKELY` (not a match): zero shift, no moved block, >= 4 blocks matching with a mean score >= the threshold, but too few of the others to prove it ("very likely home"). The boot check passes it to the motor layer as answer 3; `hal_ptz_start` then runs the SHORT calibration instead of the full one: the driver counters are declared at pan home + 400 and tilt home + 300 (instead of 4000 / 2160 + a second 300 tilt sweep), both axes walk to their stops (about 1300 + 300 and 1275 + 400 steps instead of 2160 + 300 and 4000: a quarter of the grinding, about 24 s instead of ~35-60 s in the model), the head goes home with the usual approach frame, and the scene check is asked AGAIN at home (`cold_check` a second time): only a MATCH trusts the position. Anything else (the proof fails, the sweep fails, a cancel) falls through to the full calibration from wherever the head is. Host test (`rot2k_hal_ptz_unit.c`): likely + proof (the proof looks at the mechanics): the counters are declared at 1675 / 1600, state home, counted as a calibration, 3 driver inits; the proof fails: the full calibration runs and ends at home; the head far from home (3000/1900) when the check says "likely" (a lie): the proof fails, the full calibration puts it right.

*Risk analysis.* The calibration works because a counter declared at X >= the real position makes the motor walk X steps and end ON the stop. If the head is within 400 / 300 steps of home that is true and the stop is reached; if the head is further away (moved while off) the short sweep ends before the stop, the counters are then wrong by the difference and the "go home" lands at the wrong place. That is why the short calibration is never trusted by itself: the scene check at home is the proof of the PHYSICAL position (an image against the reference taken at home), independent of the counters. Residual risks: (1) a false proof (the same risk class as the boot check itself: a wrong view accepted; unchanged thresholds, the proof needs the strict MATCH incl. the new rule); (2) the extra motion happens in a case the old code answered with a full calibration anyway, so the head moves in both; the short sweep grinds less against the stops (400 / 300 steps instead of up to the full travel); (3) the sweep of a head that was really far away is shorter than the stop distance: harmless, nothing is trusted until the proof, and the full calibration follows; (4) a wrong "likely" from a fixed pattern: the proof fails (the pattern is not a MATCH either), the full calibration follows, ~25 s lost. Soft range, stall detection, cancel, travel budget: the same safe-motion layer as before; no new driver request; flash: the proof's MATCH bumps the matched-boots counter exactly like a normal match (one store write), nothing else.

- **Review round 2 of the short calibration.** (H1) The frame files and the trusted flag stay INVALID / untrusted until the strict proof: `hal_ptz_start` runs `calibrate_x(p, 1)` with `defer_frames`, invalidates the files and untrusts the position right after it, asks `cold_check`, and only a MATCH writes the files (`frames_save`) and trusts it; a cancel, kill or failed proof leaves nothing adoptable (a restart calibrates). Tests: `rot2k_hal_ptz_unit.c` (files invalid at proof time, valid after a proof; a cancel during the proof: ECANCEL, files invalid, the next start calibrates and ends at the real home). (H2) A verified short calibration is not a full calibration: it does not count in `hal_ptz_cal_count` (the matched-boots counter keeps counting toward the periodic full calibration: a LIKELY boot does not zero it, the proof's MATCH adds one) and does not reset the travel; at most one flash write per boot (the proof's boots store; the screened-verdict store-back is skipped for a LIKELY boot).

## 2026-11-18 Position drift from lost steps: fewer lost steps, and the scene corrects the counters (host-tested, NOT yet run on hardware)

**Finding (hardware, 2K).** After about 114,000 steps of continuous tracking since the last calibration (fast moves at divisor 2 / 3, diagonals, frequent direction reversals while following a close person) the head had drifted physically DOWN: at the declared home 1275/1300 the view showed the desk instead of the wall and the window. A recalibration restored the right view at 1300. The motor is an open-loop stepper: the driver counters advance while the rotor stalls or slips, so steps are lost silently and the counters are only a belief. Two measures: lose fewer steps (A), and notice and fix a drift when the head is home (B).

### A. Fewer lost steps (the continuous mode, `ptz/akam_ptz_safe.h`)

All of it is in the safe layer; the tracker asks for a direction and divisors as before and the layer walks the motor there. Config keys of `ptz.conf` (strict, optional):

| key | default | meaning |
|---|---|---|
| `tilt_min_div` | 3 | the tilt axis and both axes of a diagonal never go faster than this divisor (the pan alone may use 2). Fast tilt and fast diagonals are the likely culprits (the tilt carries more load). Clamped silently, 2..20 |
| `settle_ms` | 100 | a REVERSAL on an axis stops the motor, waits this long with nothing commanded, then starts the opposite direction (0..1000; 0 = the old instant reversal) |
| `ramp_ms` | 50 | a speed INCREASE walks the divisor ladder 20, 10, 6, 4, 3, 2 one rung per this long as successive commands (the driver takes a new speed mid-move: measured); an axis that starts from rest starts at divisor 10 (or the wanted one when slower). A decrease applies at once. 0 = jump, else 50..500 |
| `px_pan_x10`, `px_tilt_x10` | 96, 96 (were 125, 146: measured 2026-11-18, see drift.md) | steps per scene thumbnail pixel x10 (see B) |

- **Reversal.** `ptzs_cont_apply` compares the wanted signs with the commanded ones per axis: an opposite sign is a reversal. It sends the stop request, commands nothing for `settle_ms` (the stall / uncommanded-motion detectors get `settle + 150 ms` of grace), then the new direction starts from rest, i.e. through the ramp. Both axes of a diagonal pause together (a stop is global). Dropping an axis was already "stop first".
- **Ramp.** One rung per poll (50 ms): 20 -> 10 -> 6 -> 4 -> 3 takes 200 ms. Every rung is a normal move command, so the driver's behaviour is the one already measured (a new command takes effect at once). Stall detection restarts at a speed change like at any command change.
- **Travel budget, weighted.** The controller's travel budget (`ptz.travel_since_cal`, the scene-reference limit, the patrol budget, the tracker's "lost chain skips its local sweep") now counts what hurts the stepper: every polled step of a continuous session costs 1 at divisor >= 4, 2 at divisor 3 and 3 at divisor 2 (`PTZS_W_DIV3`, `PTZS_W_DIV2`), and every axis reversal adds 150 steps (`PTZS_REV_COST`). The reversal count is kept separately (`hal_ptz_reversals`, `c->nrev`; plain steps in `c->travel_raw`). Absolute moves (`goto`, nudges) still count their net displacement. Effect: the budget of 200,000 is reached sooner by a tracking pattern with reversals and fast moves, which is what loses steps; the numbers are a first guess, to be tuned from the drift log below.
- **Also fixed on the way.** `ptzs_axis_goto` treated a counter ONE below the soft minimum as "left the soft range": that is the driver's own 0 of the shifted frame (offset = soft min - 1), reached legitimately by a move to the soft minimum (the park position 1275:300 is one). Now only a counter below that fails.

### B. Visual drift correction (`ptz.c`, `scene.c`, `scene_check.c`)

When the head returns HOME (the tracker's home at the end of a tracking chain, `ptz.home`, a patrol move to home, a guard return) and has been idle for `A2K_PTZ_DRIFT_IDLE_MS` (2 s: no motion, no person seen: `a2k_ptz_note_person()` is called by the detection feed), and at least `A2K_PTZ_DRIFT_MIN_TRAVEL` (1500, weighted) steps were travelled since the last check or calibration, the live thumbnail is compared with the home reference (the one the boot check uses) over a WIDER displacement range. Preconditions that make it SKIP (nothing is judged, nothing changes): scene check off, no reference or one keyed to another position than the configured home, another day / night mode than the reference, a reference that is featureless, repetitive or not screened at this threshold, a featureless live view, no frame newer than the last motion plus `A2K_PTZ_DRIFT_FRAME_MS` (1.5 s). A check that is due while a session / command starts is dropped; if the command ends away from home it waits for the next return.

`sc_drift` (`scene_check.c`) is the same comparison as the boot proof (`compare_core`) with a search radius of 8 thumbnail px and the common displacement allowed anywhere in it (the "unique peak" margin is measured outside a 2 px box around the best displacement, not around zero). The evidence rules are the strict ones of `sc_decide`: a unique peak, at least 4 SAME blocks, 2/3 of the voting blocks (half-votes for doubtful ones), a third of the blocks detailed in both, the mean score, the weak pool. Verdicts:

| verdict | meaning | action |
|---|---|---|
| OK | strict proof at displacement (0,0) | nothing (counters right); the travel mark restarts |
| SHIFT | strict proof at (dx, dy), `abs(dx), abs(dy) <= 6` px | correct the counters (below) |
| RECAL | no strict proof anywhere (mismatch, no unique peak, another scene, a repetitive live view), or the proven displacement is beyond 6 px | mark untrusted, schedule a full recalibration |
| INCONC | the best peak is (0,0), >= 4 blocks peak there, at most half as many vote a displacement, mean score >= threshold, but the strict vote fails (a person, the light) | nothing; the counters are probably right; retried after 30 s, at most 4 times per return |
| SKIP | cannot be judged (see above) | nothing |

In the glue (`hook_drift`) a SHIFT or RECAL verdict needs a SECOND, later frame to give the same verdict and displacement; otherwise it is INCONC. The reference is only READ: no flash write at all (the drift state is RAM).

**Direction and conversion.** The reference content moved by (dx, dy) px (right / down positive) means the head is physically at home + (`A2K_PTZ_DRIFT_PAN_SIGN` * dx * px_pan, `A2K_PTZ_DRIFT_TILT_SIGN` * dy * px_tilt) steps, rounded. On the 2K the pan counter + moves the picture content to the RIGHT (the measured need of `invert_pan=1` in the tracker: a person on one side turned the camera away with the naive sign, so a higher counter looks further left and the content moves right) and the tilt counter + looks UP (content moves down: the measured tilt test of 2026-11-16), so both signs are +1. THESE SIGNS ARE DERIVED FROM THOSE MEASUREMENTS, NOT MEASURED BY A DRIFT TEST: a wrong sign cannot do harm (see "Correction"): the correction is then not confirmed and a recalibration is scheduled. The steps-per-pixel factors: 12.5 pan, 14.6 tilt per 80-px-wide thumbnail pixel (about 75 / 90 steps for the 6 px limit), `ptz.conf px_pan_x10 / px_tilt_x10`, derived from the lens model of the stock numbering (4 source px of the 320-px detection frame per thumbnail px). Both need a check on the camera.

**Correction.** `hal_ptz_declare(pan, tilt)` re-declares the driver counters (the stock init-limits + init-position path, 2 s, no motion, the /tmp frame files rewritten) to the measured position; then the head is moved back to home (a few steps, the normal safe `goto`), and a second check must give (0,0): "no physical move" applies to the declare, the move home is the verification. Confirmed: `ptz.drift_corrections` + 1, the position stays trusted, `drift_corrected` stays set (RAM) until the next real calibration: **no scene reference is captured or written from a corrected or doubted position in this run**. Verification inconclusive (a person came in): the correction stands, the check is repeated later. Not confirmed (a displacement again, or a failed declare / move, or the corrected target outside the soft range): untrusted + recalibration.

**Recalibration.** The position is marked untrusted at once (`ptz.recal_reason` says why; no reference writes meanwhile) and a full recalibration is queued for when the head is idle: no person seen and no tracker / patrol move for `A2K_PTZ_RECAL_IDLE_MS` (120 s). It is not a manual command (the patrol hook is not called). At most one per `A2K_PTZ_RECAL_GAP_MS` (1 h) and `A2K_PTZ_RECAL_MAX` (2) per run: a reference that never matches (wrong from the start, the room changed) must not calibrate forever; past the limit the position stays untrusted for the run (logged). A successful calibration clears the flags and the normal reference rules (existing: the reference is checked and written only after a real calibration, a changed view at most once per 24 h) apply again; the only flash write this feature can cause is that existing reference rewrite.

**Manual continuous motion (ONVIF / API / MQTT) and the drift machine.** `a2k_ptz_vel_manual` runs in the same session as the tracker's, so it gets the safe layer's reversal settle, speed ladder and tilt cap (the mapping's tilt divisor 2 is effectively 3), and its steps are weighted in the travel budget. A manual hold counts as motion activity: the drift check and the drift recalibration do not start while it is live and their idle timers (`A2K_PTZ_DRIFT_IDLE_MS`, `A2K_PTZ_RECAL_IDLE_MS`) count from its end (the last command plus its lease, or the Stop). While a drift recalibration runs, and during the shift correction (re-declare, move home, re-verify), the manual gate answers NOTREADY (the same fault as during any calibration) and a manual Stop does not cancel the correction. **Untrusted position: manual moves are allowed** (the user is the judge of what the picture shows; the soft range still applies; nothing is written to the flash, no scene reference is written meanwhile); `ptz.untrusted` and `ptz.recal_reason` show it, and a pending recalibration runs after the hold and the idle time. Tests: `tests/rot2k_ptz_cont_unit.c` `integration_cases`.

**State and diagnostics (RAM only).** `GET /state`: `ptz.drift_last_px` ("none", then "dx,dy": the last conclusive displacement), `ptz.drift_corrections`, `ptz.recal_reason`. Every check logs one line (`ptz: drift check: <verdict> (<why>), displacement dx,dy px, SAME n of m blocks, moved k, score s`), the controller logs the action (corrected from / to counters, untrusted + reason, recalibrating), and the thumbnail used is dumped to `/tmp/akam_scene_dbg/drift.gray` (+ `.txt`, overwritten). Tune from these lines: if `shift` verdicts recur after fewer than 5000 weighted steps, lower the budget and / or raise `tilt_min_div`.

**Honest limits.** (1) The check only sees the displacement of the VIEW against the reference: a reference taken at a drifted position would make it trust the drifted position; the reference is therefore only written after a real calibration, as before. (2) A drift of more than 6 px (75 / 90 steps) is not corrected (a recalibration), and a scene without usable texture cannot be judged at all (SKIP). (3) A real displacement of less than 1 px (12 steps) is not seen. (4) Everything here needs a hardware run: the signs and factors above, and the effect of the settle / ramp / cap on the lost-step rate (a long tracking soak: compare the drift log before and after).

**Motor-safety review follow-ups (2026-11-18, host-tested, NOT yet run on hardware).** (1) A pending drift recalibration never runs while the head is parked (privacy or manual park): it stays pending and runs once the head is back (a park scheduled after the RECAL verdict is not undone); with `ptz_cal=off` it is dropped and logged, never run. (2) During the SHIFT window (declare, move home, re-verify, about 2 s) `submit2` refuses every queued command (step, home, park, goto, nudge, recalibrate, tracker home) with `A2K_PTZ_E_NOTREADY`, the code of the manual gate, and none cancels the correction. A Stop has nothing to stop (manual motion is refused, the window itself is never cancelled, nothing is pending), so it is a no-op; the user repeats it after the window. A correction that ends with ECANCEL (only the shutdown) is "check again later" (`drift_due`), not a recalibration. (3) A SHIFT whose re-verify was inconclusive / skipped leaves the correction unconfirmed: a further SHIFT verdict schedules a recalibration instead of a second declare; cleared by a re-verify or check that matches (0,0) or by a calibration. (4) The signed sum of corrected steps per axis since the last calibration is kept (RAM); a correction that would take it beyond +-`drift_cum_max` (`ptz.conf`, default 120, 10..2000) schedules a recalibration instead; reset by a calibration. (5) `tilt_min_div` now also caps the tilt in `ptzs_axis_goto` (goto, nudge, home, park) and in the tilt / diagonal steps of `hal_ptz_step`, and a goto / nudge / step requested faster than divisor 3 is weighted in the travel budget like a continuous session at the divisor really used (pan x3 at 2, tilt x2 at the floor 3); ordinary moves (divisor >= 3, home / park returns) keep counting 1 per step. Tests: `rot2k_ptz_cont_unit.c` `review_cases_drift` and `mitig_cases`.

**Tests.** `rot2k_ptz_cont_unit.c`: reversal = stop + settle + opposite (also `settle_ms=0` = the old one), the ladder 10 6 4 3 2 at 50 ms, direct start of a slow speed, tilt / diagonal cap and `tilt_min_div`, config refusals, weighted travel (x1 at 6, x2 at 3, +150 per reversal, reversal count), and the controller on a fake driver with a fake CAMERA whose view follows the mechanics (not the counters): injected lost steps are measured, corrected, the head is physically home again and the sign / conversion are right; no check with a person, during a session, right after a calibration; inconclusive and skip do nothing; a 16 px shift marks untrusted, waits for the idle time, recalibrates and is trusted again; no reference capture while untrusted. `rot2k_scene_unit.c` `test_drift`: the real wall and the real dusk pair shifted by 0..10 px (exact displacement for the wall in all 15 cases, never a wrong proof, 7..10 px always RECAL), mode difference / no reference / repetitive or unscreened reference / featureless live view = SKIP, another scene and a repetitive live view = never a correction, a person over 5..6 of 12 blocks = inconclusive. `rot2k_scene_glue_unit.c`: the hook with a feeder thread (a second frame confirms, a disagreeing second frame is inconclusive, stale frames skip, no flash write).

## 2026-11-18 Tilt accounting audit: counted vs truly moved (host-tested, hardware cause NOT yet isolated)

**Observation (hardware, 2K).** About 10 min of live tracking with dozens of close-range vertical scans (up 250 / down 300, divisor 5) plus continuous tracking moves: back at home the counters read 1277/1297 then 1274/1303 but the view was physically clearly TOO HIGH; the visual drift check found no peak (offset beyond its 8 px = about 117 tilt steps window), so the position was untrusted and a recalibration fixed it. Before the mitigations (settle, ramp, tilt floor) the opposite happened: after about 114,000 tracking steps the head had slid DOWN.

**Audit result: there is no software counter to be biased.** The position the software uses and reports is the DRIVER's own step counter, read back by `ptzs_read_axis` (ioctl `0x801c0c01`, selector 0 / 1) and converted by the constant frame offset (`real = raw + soft_min - 1`). The driver interface is not a step count or a duration: a move is a command word 0..7 plus a speed divisor per axis, "walk the counter to its extreme (soft limit) at one step per `divisor` ticks of a 1 kHz timer"; the stop request ends it. Nothing in `hal_ptz.c`, `akam_ptz_safe.h` or the controller writes a position counter, and nothing derives a distance from a time:
- `hal_ptz_step` (the 897c027 change): the duration `step * divisor` only decides WHEN the stop is sent; the final position is the counter read after the stop. Raising the tilt divisor to `tilt_min_div` and scaling the duration changes how long the motor runs, never what is counted. The weighted travel (`wtilt`, `PTZS_W_DIV3`, `PTZS_REV_COST`) feeds ONLY the wear budget (`hal_ptz_travel`: scene-reference limit, drift-check trigger, patrol), not the position.
- `ptzs_axis_goto` / nudge / home / park: polled against the counter, stopped when the counter reaches the target (tolerance 3, accepted within 10).
- Continuous sessions (tracker velocity, the VSCAN legs, ONVIF / API velocity): `ptzs_cont_*` only sends direction + divisors and polls; reversal settle, ladder rungs and the tilt cap change the command stream, the counter stays the driver's.
- The only places that WRITE the counters are the boot / drift calibration (`drv_init`, `hal_ptz_declare`); none runs during tracking. The drift SHIFT correction (declare from a measured pixel displacement) was not involved in the observation (the check found no peak).
- Up and down are symmetric in code: same divisor (VSCAN uses `rate_k / NL_VS_DIV` both ways), same ramp, same weights, same stop path. Rounding cannot accumulate: nothing is rounded into the counter.
- Counter reads are up to one poll (50 ms, about 10 steps at divisor 5) stale inside a session; the tracker uses them for decisions only, and the final read of every motion is taken after the stop (`ptzs_cont_end` waits 60 ms first).

So a counter that disagrees with the picture can only be PHYSICAL: the counter counts the pulses the driver issued, not the steps the rotor made (open loop, no encoder). A drift "too high" with counters at home means the head made fewer DOWN steps than were counted (or more up steps than counted, which a stepper cannot do): missed steps (or stiction / backlash) on the down legs, or a net loss during reversals. Quantification: the drift check window is 8 px x 14.6 = about 117 steps and it found no peak, so the net error was at least about 120 tilt steps; over about 40 to 60 scan cycles that is at least 2 to 3 steps lost per cycle (about 1 % of the 550 steps travelled per cycle). The fake-motor model below produces exactly 0.

**Unit test** (`tests/rot2k_ptz_cont_unit.c`, `tilt_account_cases`): the fake driver now keeps an independent count of the steps its motor really executed (`M.exec`, `exec_pos`, `exec_neg`, in the stepping loop, not derived from the counters). 50 VSCAN-like cycles (up 250 / down 300 / up 50 at divisor 5, one session per leg, 600 ms dwell between) interleaved with 100 tracking-like sessions (reversals every 100 ms, diagonals, divisors 2..6, so the ladder, settle and the tilt cap are all exercised): every one of the 150 legs has `counter delta == executed steps == mechanical delta`, and at the end `true - counted = 0` (21192 steps up, 20621 down, 72 driver move commands per cycle including the tracking sessions). It PASSES on the current code, as the audit predicts; it fails the day anything starts deriving the position from a request or a time, or crediting steps differently up and down. It cannot model missed steps (their rate is the unknown).

**Likely physical causes (to be measured).** (1) Steps missed by the rotor on the down legs: the speed ladder starts a tilt from rest at divisor 10 and reaches the floor 3 (333 steps/s) within 200 ms, VSCAN runs at 5 (about 220 steps/s); a too-fast rate or a too-short ramp loses steps in either direction, the earlier finding (head DOWN, divisors 2 / 3) being the up direction against gravity. (2) Gear backlash / cable stiction: each reversal swallows some steps; over a leg pattern with unequal legs and dwells the error is bounded per reversal, but it is not zero and the tracking sessions reverse often. (3) Holding torque released at a stop: the head sags a few steps under its own weight (this would make the head LOWER, the opposite of the observation, so it is unlikely the main cause).

**Hardware experiment (user at the camera; nothing is flashed, the head is moved with `akam_ptz` / the API within the soft range).** Per run: calibrate (reboot or `recalibrate`), go to home 1275/1300, run N identical cycles, go home by `goto`, read the counters (`pos`) and the drift check px (`ptz: drift check` log line / `GET /state ptz.drift_last_px`; the view against the home reference, 1 px = 14.6 tilt steps), then recalibrate before the next run. Cycle = tilt up 250 steps, down 300, up 50 (moves of `steps * divisor` ms: `akam_ptz move 0 <ms> <div> <div>`, then `move 4 ...`, with 600 ms between legs; the tool needs the app stopped, so alternatively drive the legs with the API velocity command and a stopwatch, or a small script of `ptz.vel` calls). Matrix: A) divisor 5, N = 0 (baseline px), 10, 20, 40; B) N = 40 at divisor 10, then at 3 (does the drift scale with the rate: missed steps; or stay constant: backlash); C) N = 40 with up/down legs only in one direction pair (down 300 then up 300) and N = 40 with 1500 ms dwells (does the pause matter); D) N = 40 down-only legs of 100 steps then one up of 4000 / 100 back to isolate one direction. The drift per cycle in steps is `px * 14.6 / N`; the sign (head high or low) says which direction loses steps. If the drift grows with N and shrinks at a slower divisor, set `tilt_min_div` and the VSCAN divisor from the measured loss rate; if it is independent of the rate, the cause is backlash and the remedy is to end every tilt excursion with a move in the same direction (a final approach "from below"), not a slower speed.

## 2026-11-18 Review follow-ups: restart after a dropped axis, API answers (host-tested, NOT yet run on hardware)

- **S1 drop-stop restart.** Best candidate for the tracking-only step loss: when one axis left a diagonal the safe layer sent a stop and re-sent the other axis at its cruising divisor at once (`cs` kept the old speed): a full-speed restart of a head that had just stopped, never happening in single-axis up / down cycles. Now an axis leaving the command takes the reversal path (stop, `settle_ms`), and the remaining axis restarts from rest through the first ladder rung (10, 6, 4, 3). `settle_ms = 0` / `ramp_ms = 0` keep the instant behaviour. Test: `drop_cases` in `tests/rot2k_ptz_cont_unit.c` (fake driver: first divisor after the drop is 10, at least 100 ms after the stop).
- **API answers that did not match the motion.** The measurement script saw `ptz.move` (about 1 of 3) and `ptz.home` answered 200 {"ok":true} while the head did not move (home left pan at 1324 instead of 1275). Found: a manual nudge toward a soft limit with no room is clamped to nothing by the HAL and was accepted. It is refused now (`A2K_PTZ_E_LIMIT`, API 409 `limit`). A busy motor was already 409 `unavailable`. NOT reproduced on the fake driver: an accepted nudge or home dropped silently (nudges 0.6..1 s apart, home 0..0.6 s after a nudge all move / end at home), so the home case (49 steps off) may be a step loss or backlash of the home move itself rather than an unanswered command; to verify on the camera by comparing the `ptz: nudge` / `ptz: home` log lines with the API lines.
- **Measured tonight:** about 2 steps per px at 320x180 on both axes; tilt backlash +50 steps -> 33 px, -50 steps -> 21 px for the same counted steps; home after an up move is about 3 px (6 steps) from home after a down move.

## 2026-11-20 Tilt slip again (head TOO HIGH at counters 1277/1299): analysis of the recorded session and the tilt-only re-home (host-tested, NOT yet run on hardware)

**Observation.** After a 173 s tracking session (raw recording `.local/logs/track-sessions/20261120-session1`, `travel_since_cal` 127027 weighted steps) the counters said home and the camera looked at the ceiling; a full recalibration fixed it. The drift check had not given a verdict (`ptz.drift_last_px` none, 0 corrections).

**What the recording shows (counters only; it cannot show a slip, the counters count pulses).**
- The tilt never went near a stop: range 616..1409 against the soft range 300..2100 (the mechanical bottom stop is 0 and the top about 2315), so no step was lost by driving into a stop.
- Speeds: tilt peaked at 305 steps/s (divisor 3, the `tilt_min_div` floor), typical 100 to 200 steps/s (divisor 5 to 6), never faster than the settle / ladder / cap allow, and slower than the stock app (speed 1, about 1000 steps/s; the stock calibrates at speed 1, ours at 3).
- Reversals: 9 tilt reversals in 173 s, each preceded by the 100 ms settle; no fast reversal pattern. The tilt travel was 3402 steps (1446 up, 1956 down in the ring), net -510 before the home return.
- The pattern that is NOT symmetric: the close-loss VERTICAL SCAN ("close-scan-up" 250 then "close-scan-down" 300 at divisor 5) followed by a continuous tracking move down: DN legs of 436, 645 and 737 steps in exactly 4.0 s each (up legs 240 / 70 / 113), i.e. the DOWN direction carries about 35 % more travel than the UP one (1956 against 1446). The earlier audit (2026-11-18) already found the same sign of error ("too high" = down steps lost or backlash on the down legs, 2 to 3 steps per scan cycle) and the same absence of a cause in the software counters. It is a physical open-loop effect; a software cap cannot remove it, only bound it. The tilt re-home below bounds it.

**Why the drift check did not give a verdict (code, not proven on the camera: the log of the run holds no `ptz: drift check` line, only the decision log).** The check runs only when (1) a motion ended AT home with the position trusted (`drift_note_motion_locked`), (2) at least 1500 weighted steps were travelled since the last check, (3) nobody was seen and nothing moved for 2 s, (4) a home reference exists, is keyed to the configured home, is for the same day / night mode, and is not featureless or repetitive, (5) the live view has detail and a frame newer than the last motion + 1.5 s exists. A head that looks at the CEILING (a featureless view) fails (4)/(5): the verdict is SKIP, which only logs and leaves `drift_last_px` at none, schedules nothing, and restarts the travel mark, so nothing is retried until another 1500 steps are travelled. The same holds when the slip is larger than the 8 px (about 77 steps) window: RECAL would have set `ptz.untrusted` (it was not reported). So the visual check is blind in exactly the case that matters (large slip, empty view); the re-home does not depend on the picture.

**Fix: `ptz_tilt_rehome=idle` (default), `ptz_tilt_rehome_gap_min` (default 30).** `hal_ptz_tilt_rehome` (hal_ptz.c): the head walks the FULL stroke DOWN at the configured calibration speed like the boot calibration (counter declared at 2160 and walked to 0, then declared 300 and walked again: on the bottom stop from any real tilt, however large the slip), the counter is re-declared as real 1 (the stop, like a calibration) and the head goes UP to the tilt home, last leg from below. The pan is never commanded and is re-declared with its own value. No flash write. The controller (ptz.c) arms it when a motion ends at home with the position trusted and at least 3000 weighted steps since the last re-home / calibration, starts it after 10 s without a person seen, tracker command, manual hold or motion (and after a pending drift check), at most once per gap (an interrupted attempt counts), never parked / privacy / `ptz_cal=off` / untrusted. A person (`a2k_ptz_note_person`), a tracker command or a user PTZ command cancels it: the motor stops and the believed position of before is kept (counter minus the walk: the slip is what it was, nothing worse); a failure schedules the normal recalibration. A completed re-home restarts the drift mark and the signed correction sum of the tilt. Cost: about 25 s of head motion (the full stroke down at speed 3 is 7 s plus 1 s, the driver inits 2 s each, 4 s up) every 30 min at most, only when the head was away and the room is empty; it wears the tilt by one full travel.

**Tests** (`tests/rot2k_ptz_cont_unit.c` `rehome_hal_cases`, `rehome_ctl_cases`, fake driver with mechanical stops): a counter error of +150 and -150 injected on the tilt is gone after the re-home (the head physically at the stop-derived home, counter equal to the true tilt, pan never executed a step); a cancel on the way down keeps the believed position (counter - physical stays -150); with a person in view for 5 x the idle time, during a tracking session, and when a session ends away from home: no re-home; quiet at home: it runs once; a second return home inside the gap: none; after the gap: it starts, a person cancels it (counted, slip unchanged), a tracker velocity command cancels another one.
