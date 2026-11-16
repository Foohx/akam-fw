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

T6. Motion test with `akam_ptz` (built: `tools/build/bootstrap.sh && firmware/lsc-rotatable-2k-ak3918av130/ptz/build_akam_ptz.sh`,
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

- `akam_ptz` exists (`firmware/lsc-rotatable-2k-ak3918av130/ptz/`); its request numbers and the argument layout are
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

Files (`firmware/lsc-rotatable-2k-ak3918av130/app/`): `hal_ptz.h/.c` (portable engine: calibration, saved position, goto / step /
nudge / cancel on top of `ptz/akam_ptz_safe.h`, reused unchanged except the new defaults, `ptzs_conf_parse_over` and a `PTZS_QUIET` switch),
`hal_ptz_drv.c` (Linux driver: node, ioctl, atomic files), the 2K descriptor in `akam2k_hal.c` (`/dev/tymotor`, command words 0 up, 2 right,
4 down, 6 left, the measured range above, park 1275/300 (down; was 1275/1750), tilt soft max 2100 (was 1750; head level-up at about 2104, top stop 2290..2340), speed 3, button step 200), `akam2k_ptz.h/.c` (one thread, one pending command).
Unit test: `tests/rot2k_hal_ptz_unit.c` (simulated motor with hard stops; part of `tests/rot2k_overlay_test.sh`).

**Files on the camera** (`/config/akam`, jffs2): `ptz.conf` optional, same strict keys as the tool (`pan_min pan_max tilt_min tilt_max pan_home
tilt_home pan_step_max tilt_step_max speed`); a bad one switches PTZ off (motor never touched). `ptz.pos` = `pan=N tilt=M` (real steps) from a
clean stop, or `unknown`. DIAGNOSTIC / last known value only: it is never used to skip the calibration (see Boot). Writes: `unknown` once per burst of motions (only when the file does not already say so), the position once after
2 s without a command (and at stop), atomic (temp + fsync + rename), skipped when identical; a directory that does not exist is never created
(one warning).

**Boot** (`ptz_cal=auto`, default): (0) a stop request, then (1) the driver already holds our frame file (`/tmp/akam_ptz_app.frame`, written
next to `/tmp/akam_ptz.frame`) and non-0/0 counters (the app restarted in the same boot): adopted, no init; (2) otherwise, i.e. EVERY COLD BOOT, the
calibration, then state home. A saved position is never declared to the driver: the head can move outside the app (by hand, `akam_ptz`, a stock
firmware boot) and the open-loop counters cannot detect it (safety review, MEDIUM; the stock calibrates at every boot as well). The adoption is
refused (so the next start calibrates) after a stall or a failed calibration (the app invalidates both frame files), and after any `akam_ptz`
motion / `init` / `restore` (the tool invalidates `/tmp/akam_ptz_app.frame` and writes `unknown` to `/config/akam/ptz.pos`). `ptz_cal=off`: the motor is not initialised, the state
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

**API** (`akam2k_ptz.h`): `a2k_ptz_toggle_park()` (parked -> home, else park), `a2k_ptz_nudge(dx, dy)` (relative steps, summed while queued,
never cancels), `a2k_ptz_command(A2K_PTZ_*)`, `a2k_ptz_info()`, `a2k_ptz_privacy_active()` (one volatile read on the video path). Motor
commands run on the PTZ thread only; one motion at a time; a new home / park / step cancels the running motion (stopped within ~50 ms, position
kept) and replaces the pending command; every command is refused while calibrating.

**akam2k.conf keys** (passed by `own_app_run.sh`): `ptz_cal` auto|off, `ptz_park` PAN:TILT (default 1275:300), `ptz_privacy` 0|1, `ptz_scene_check` 0|1, `ptz_scene_min` 0.80..0.99, `track_invert_pan` / `track_invert_tilt` 0|1.

**MQTT / Home Assistant** (same filtering as the LED controls: retained commands ignored, 20 commands per 10 s, strict payload): buttons
`ptz_left` `ptz_right` `ptz_up` `ptz_down` (one step of 200 steps), `ptz_home`, `ptz_park`, `ptz_recal` ("Recalibrate PTZ": the long calibration now; refused with `ptz_cal=off`) (command topic `BASE/<id>/set`, payload exactly `PRESS`);
sensor `ptz_state` (enum home / parked / moved / calibrating / unknown); diagnostic sensors `ptz_pan`, `ptz_tilt` (steps, only while known).

**Hardware test plan (user at the camera, a hand on the power cable)**: (1) with `ptz_cal=off` boot: log says "not initialised", HA state
`unknown`, buttons do nothing. (2) `ptz_cal=auto`, no `/config/akam/ptz.pos`: watch the calibration (tilt down, pan left, then home); abort criteria:
grinding louder than the stock boot, a motion that does not stop, the log line `calibration FAILED`. (3) after it: buttons in all four directions
(about 200 steps), park (looks up), home; `cat /config/akam/ptz.pos` 3 s later. (4) reboot: it calibrates again (the saved position is not used), even when the head was moved by hand before. (4b) `akam_ptz move 2 200` while the app is stopped, then restart the app: it calibrates (no adoption). (5) `kill -9`
the app during a motion: the file says `unknown`, the next start calibrates; `kill -TERM` restarts the app: adopted, no calibration. (6) `ptz_privacy=1`:
park, the RTSP client freezes, home, the video resumes within one GOP.

## Cold-boot calibration and cancel race (safety review follow-up)

- Cold boot always calibrates; `ptz.pos` is diagnostic only; same-boot app restart adopts (own frame file + non-0/0 counters); a stall or a
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

**Reference** (`/config/akam/ptz.scene`, < 4 KB, text header `scene1 <mode> <pan> <tilt> 80 48 <boots>` + 3840 raw gray bytes; or `invalid`): the position and an
80x48 grayscale thumbnail (box filter 4x4 of the Y plane of the 320x192 detection frame, taken at most every 500 ms by `a2k_scene_feed`, no extra capture),
with the day / night mode at capture and the number of matched boots since the last full calibration. Written in TWO steps on the PTZ thread, so that the
thumbnail and the position belong to the same moment (safety review M1): (1) once the position is known and the motor has been quiet for 6 s (the AE has
settled) the current view is kept in RAM as the candidate; (2) 6 s later (or at the app shutdown, if the candidate is more than 3 s old) the PTZ thread
RE-READS the driver counters (they must still equal the cached position, within 4 steps) and the scene module writes the candidate only if the current
view still matches it (same ZNCC + block rule, same day / night mode). A view that changed, counters that differ, or no settled candidate: nothing is
written (up to 3 retries, then the next cold boot calibrates). Atomic (`akam2k_store`), at most one write per 5 min after the first one (flash wear, a
tracker moves often); a view without detail is never saved (and not logged as saved).

**Boot decision** (`app/scene_check.c`, pure, `tests/rot2k_scene_unit.c`, `tests/rot2k_scene_glue_unit.c`): BEFORE any motor command the PTZ thread waits
for the first frame + 4 s + 6 frames (AE settling; a day/night change restarts it; 30 s timeout), then compares: both images must have detail (std dev >= 8
gray levels AND mean gradient >= 1.5), the day/night mode must be the same, the best zero-mean normalised cross-correlation over a +-2 thumbnail-px shift
(about +-8 source px) must be >= `ptz_scene_min` (default 0.90, lowest accepted 0.80), AND (M2) at least 8 of the 4x3 blocks must each have detail in both
images and match on their own (>= min - 0.10): a pattern fixed to the camera (lens dirt, an overlay, a sensor artefact) correlates perfectly whatever
the scene, but covers only some blocks. Match: the saved position is declared to the driver (final shifted frame, no motion, no calibration), EXCEPT when
it is the park position (within 15 steps; the parked view is the least informative one): that calibrates. Every 10th matched boot is a full calibration
anyway (L1: the counter `boots` lives in the reference and is rewritten at each match; if it cannot be written, no match). Anything else (no reference,
low detail, mode differs, mismatch, no frame, error, timeout, position outside the soft range) is the full calibration as before. The score and the
decision are logged (`ptz: scene check: MATCH, score 0.987 >= 0.900 (boot 3 of 10 ...)`). `ptz_scene_check=0` disables everything; without the
detection (`detect=0`) there is no frame source: always calibrates.

**Invalidation** (L2: fail closed): the reference is overwritten with `invalid` when any motion starts (and before every calibration); if that write
fails the motion / calibration is REFUSED (like `ptz.pos`), so a stale reference never outlives a new position. After a stall the position is unknown
and nothing is saved; `akam_ptz` writes the marker on any motion / init (`PTZ_SCENE_PATH`).

**Calibration sweeps** (M3): the real tilt travel is about 2315 steps (top stop 2290..2340) but the declared step maximum is 2160, so one sweep could
end about 150 steps above the bottom stop. The calibration now sweeps tilt down, re-declares the tilt counter at 300 and sweeps down again (the pan
counter keeps its maximum), then pan left: four inits instead of three, the motor always ends on the bottom stop. Host test: head at the top stop.

**Limits (documented, tested)**: ZNCC ignores a global gain / offset (lights on / off, AE: matches), but a lighting change over part of the image lowers the
score; a uniform wall, a covered lens or a dark night view prove nothing (rejected: calibrate); a very repetitive pattern can match after a shift of one
period; a rotation of more than about 3 thumbnail px (12 source px) mismatches; moved furniture or people may force a calibration (safe). The one thing
that must never happen, a match on a uniform view, is excluded by the detail check on BOTH images, and a match carried by a fixed pattern by the block rule.

**Open-loop limit (accepted)**: the head is a powered open-loop stepper. Turning it by hand within the capture window after a motion (before the candidate is written) can pair a moved view with unchanged counters. It is bounded by the periodic full calibration every 10 matched boots; a restart of the app in the same boot (adoption) keeps the on-disk count (`invalid <boots>` marker), it does not restart it.

**Recalibrate button** (HA `ptz_recal`, `A2K_PTZ_RECAL`): the long calibration now from any state (reference dropped first, new one written when quiet,
counter restarted); every command is refused meanwhile; refused with `ptz_cal=off`; a cancel (shutdown) between the dequeue and the start is honoured.

## 2026-11-15 hardware follow-ups

- Park default is now 1275:300 (down, 300 steps above the calibrated bottom stop). Tilt soft max 2100 (level-up view about 2104; top stop 2290..2340).
- Person tracking: the pan direction was inverted on the 2K (a person on one side turned the camera away, pan 1275 -> 996 -> 695). The 2K now defaults
  `invert_pan=1` (akam2k.conf `track_invert_pan`, `track_invert_tilt`; tilt default 0, unverified).
