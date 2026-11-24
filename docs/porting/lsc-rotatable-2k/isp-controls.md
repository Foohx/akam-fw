# Rotatable 2K: runtime ISP / VPSS controls (colour, white balance, noise, "too red")

Status: **offline reverse engineering plus a RAM-only test hook; nothing of this ran on the camera yet** (2026-11-18). Everything below is read
from `libplat_vpss.so` (V3.1.03), `libplat_isp_sdk.so`, the stock `yy_cam` 14.1.5 and the ISP conf file `isp_sc1346_mipi_1lane_av130.bin`
(SC1346 sensor, the same bytes in the stock appfs and in our appfs tree, checked with `cmp`). Related notes: stock-firmware-reference.md ("Image
effects", "WDR call chain"), archive/rot2k-own-app-feasibility.md ("Day/night and image"), venc-live-params.md (the `--venc-test` hook this one copies),
`platforms/anyka-ak3918av130/abi.h` (how structs are transcribed: our own struct text with `_Static_assert`, no vendor header).

Confidence words: **HIGH** = read from code that validates the field (the lib prints the field name in its error string), **MEDIUM** = consistent
with the code and with the conf file bytes but not proven by a name, **LOW** = guess. "Unverified on the camera" applies to all of it until the
test plan at the end has run.

## 1. What the stock does (and what it does not)

`yy_cam` imports exactly these image calls (`readelf --dyn-syms`): `ak_vpss_effect_set`, `ak_vpss_effect_get`, `ak_vpss_get_soft_ps_level`,
`ak_vpss_set_soft_ps_param`, `ak_vpss_get_soft_wh_level`, `ak_vpss_get_wdr_attr`, `ak_vpss_set_wdr_attr`, `ak_vpss_md_get_stat`,
`ak_vpss_switch_flip_mirror`, `ak_vi_switch_mode`, `ak_vi_load_sensor_cfg`. The flip call is in section 3.2. The `effect_set` call sites (0x2a1724, 0x2a2c34, 0x2a2f2c,
0x2a3224, plus the WDR ones) use types **7, 4, 3, 1, 5** only. So the stock makes **no** white balance, CCM, saturation, hue, noise reduction or
gamma call: the picture it produces is the conf file (day / night set) with the ISP's own automatic white balance, plus brightness, contrast and
sharpness only when the user set them (default -1 = nothing applied), plus WDR 40. Our app does the same set of calls (image.c,
a2k_isp_wdr), so **a colour cast that is not in the stock picture cannot come from a missing stock call**; it can come from the IR-cut
(section 6), from the ISP set that is loaded (day / night / type 2), or from the effects we apply. Check this first with a daylight
frame of the stock and of our app side by side: the existing `test-images/rot2k/` frames are all dark scenes (the stock one,
`2026-11-14_palier5_stock_stream0`, and ours, `2026-11-15_poc3_stream0`, average R/G/B 35/25/41 versus 25/15/31: the same magenta bias, both with
G low, so in the dark the app matches the stock).

## 2. The ISP conf file (sections, colour matrix)

`isp_sc1346_mipi_1lane_av130.bin`, 282464 bytes, header text "isp_sc1346_mipi_1lane_av130_VERSION=common-v1.0.3, DATE=2026-06-10-18". It is
**three sections of 0x16fd0 bytes each**, every one starting with a 512-byte header (u32 7, "6.02", u32 sensor id 0x1346 at +0x14, build date at
+0x18, **section type byte at +0x1f** (0, 1, 2), size at +0x24 = 94160). Sections at 0x0 (type 0), 0x16fd0 (type 1), 0x2dfa0 (type 2).
`ak_vi_switch_mode(0, 0)` loads the day set, `(0, 1)` the night set; the stock's mode 2 (white light "full colour" variant) is the third.
MEDIUM that type 0 = day, 1 = night, 2 = full colour night (the colour matrices below say so).

The colour matrix block (`ak_vpss_*_ccm_attr`, 324 bytes) sits at section offset **0x3d5c** in all three. Decoded (HIGH layout: it validates against the
lib's checks, row sums are all 256 = unity, and the three blocks decode to sane values):

| section | cc_mode | manual matrix | automatic table |
|---|---|---|---|
| 0 day (0x3d5c) | **1 (automatic)** | 512 -200 -56 / -34 398 -108 / -21 -210 487 | 4 matrices by colour temperature: 3000 K 339 -168 85 / -60 256 60 / 8 -242 490, 4200 K 402 -201 55 / -44 275 25 / 24 -212 444, 5200 K 384 -185 57 / -35 264 27 / 21 -191 426, 6600 K 396 -191 51 / -29 261 24 / 15 -158 399 |
| 1 night (0x1ad2c) | **0 (manual)** | 77 150 29 / 77 150 29 / 77 150 29 (a luma matrix: mono, hence the grey night picture) | 2800 / 4000 / 5000 / 6500 K, strong red-suppressing rows (458 -275 73 ...), not used in mode 0 |
| 2 (0x31cfc) | 1 | same as day | same as day, entry 3 without the extra noise parameters (`cnoise_yth 32, gain 168`) |

So the day set is an ordinary 4-point colour-temperature CCM with the usual strong negative off-diagonals (a saturating matrix). **The CCM is
interpolated by the ISP's own colour temperature estimate**; if that estimate is wrong the result is tinted, which is why a manual WB or a gentler
matrix are worth trying (section 5).

Other tables (NR gain linkage etc.) are also in these sections but their offsets are not known: the layout of the section is the kernel ISP
driver's, not the vendor lib's. Read the live values with the hook (`get all`) instead of parsing the file. The file also carries a "style" notion
(`isp_cfg_file_get_style_id`, `/etc/config/styleId`, `ak_vpss_effect_set(dev, 6, style)`); our sections all have style 0, the stock yy_cam never
sets a style, so ignore it.

## 3. Controls

All `ak_vpss_*` functions take the ISP device first (0 here; > 3 gives 0x100) and return **0 on success, 0x100 for a NULL pointer or a bad device,
or a validation / ioctl failure**. Every setter validates the **whole** struct in user space before it writes, so a get -> change one field -> set
of the same buffer cannot put a half-valid struct into the driver (a bad field is logged by the lib and nothing is written). The kernel
side copies a struct of the driver's size from the pointer we pass: the hook therefore passes 16 KiB zeroed buffers (larger than every struct here;
the largest is the 3D NR attribute, 7076 bytes). All `get`s copy from the driver (ioctl `0x8004 49 nn`, the size field of the ioctl code is always 4:
the sizes below come from the callers' memsets and the validations, not from the ioctl numbers).

| What | Symbol (libplat_vpss) | Signature | Struct / range | get/set | Stock | Conf. |
|---|---|---|---|---|---|---|
| brightness, contrast, sharpness, hue, saturation, WDR, mains Hz | `ak_vpss_effect_set` 0x8a14 / `ak_vpss_effect_get` 0x8bb8 | `(dev, type, value)` / `(dev, type, int *out)` | type 0 hue, **1 brightness, 2 saturation**, 3 contrast, 4 sharpness (all -50..50, 0 = conf value), 5 WDR (0 / 40), 6 style, 7 mains (50 or 60), 8 flip bits | get returns the last set value (cache, 0 at first); set is delta-from-conf | 1, 3, 4, 5, 7 | HIGH (call, range), MEDIUM (type 2 = saturation, 0 = hue, see 3.1) |
| YUV effect struct | `ak_vpss_get_yuv_effect_attr` 0x19748 / `ak_vpss_set_yuv_effect_attr` 0x1954c | `(dev, void *)` 10 bytes | `y_a` u16 +0 (0..255), `y_b` s16 +2, **`uv_a` s16 +4**, `uv_b` s16 +6, `dark_margin_en` u16 +8 (0..2) | symmetric | no | HIGH (names order = validation order) |
| WB type | `ak_vpss_get_wb_type` 0x16dc4 / `ak_vpss_set_wb_type` 0x16ee8 | `(dev, int *)` both | 0 or 1; **which is automatic is not in the code** (it is the u32 at +8 of the 2016-byte WB attr) | symmetric | no | HIGH (call), LOW (meaning of 0/1: the hook takes the value it reads at start, with the AWB running, as "auto") |
| manual WB gains | `ak_vpss_get_mwb_attr` 0x18194 / `ak_vpss_set_mwb_attr` 0x182cc | `(dev, void *)` 12 bytes | `r_gain` u16, `g_gain` u16, `b_gain` u16 (each 0..4095, error text "gain must in [0, 4095]"), `r_off`, `g_off`, `b_off` s16 (-512..511); sits at +12 of the WB attr (get/modify/set of the full 2016 bytes is done by the lib) | symmetric | no | HIGH layout, **unit UNVERIFIED** (256 = 1.0x is the usual Anyka 8.8 form; the conf's own manual values, `get wb`, will show it) |
| auto WB attribute | `ak_vpss_get_awb_attr` 0x18504 / `ak_vpss_set_awb_attr` 0x18640 | `(dev, void *)` ~1992 bytes (the WB attr minus its 24 byte head) | `g_weight[]`, `y_low`, `y_high`, `err_est`, `awb_iso_track_enable`, `auto_wb_step`, `awb_stat_calib_para[]` (gr/gb/rb low/high), `colortemp_stable_cnt_thresh`, `colortemp_envi[]`, `alg_type` | symmetric | no | HIGH names, LOW offsets (not decoded) |
| AWB statistics | `ak_vpss_get_awb_stat_info` 0x15f3c | `(dev, void *)`: 9376 bytes of statistics + **38 bytes of run info at +9376** | the run info is u16 fields (current gains / colour temperature, unnamed) | read only | no | HIGH size, LOW fields |
| global RGB average | `ak_vpss_get_rgb_average` 0x16c2c | `(dev, int *r, int *g, int *b)` | sums at +36/+76/+116 of the statistics divided by the count at +156 | read only | no | MEDIUM (argument order from the stores; harmless: writes three ints) |
| colour matrix | `ak_vpss_get_ccm_attr` 0x1b054 / `ak_vpss_set_ccm_attr` 0x1ab60 | `(dev, void *)` 324 bytes | u32 `cc_mode` +0 (0..1); manual: u16 +4 (<=255), +6 (<=256), +8 (<=127), +10 `cc_enable` (0..1), +12/+14/+16 `cc_cnoise_yth/gain/slop` (<=1023), s16 `ccm[3][3]` +18 (-2048..2047); u16 `color_matrix_num` +36 (3..10), +38 (<=255), +40 (<=256), +42 (<=127); 10 auto entries from +44, stride 28: u16 `cc_color_temp` (2000..20000), `cc_enable`, `cnoise_yth`, `gain`, `slop`, s16 `ccm[3][3]` at +10 | symmetric | no | HIGH (validated, and the conf bytes decode exactly) |
| saturation attribute | `ak_vpss_get_saturation_attr` 0x1e8d8 / `ak_vpss_set_saturation_attr` 0x1e478 | `(dev, void *)` 344 bytes | u32 mode +0 (0..1), then 17 records of 20 bytes from +4 (u16 flag, four u16 <= 1023, five u16 <= 255): a gain-linked table, **no single strength field is named** | symmetric | no | MEDIUM layout, LOW semantics: use effect type 2 instead |
| hue attribute | `ak_vpss_get_hue_attr` 0x1b2e0 / `set` 0x1b130 | `(dev, void *)` 2204 bytes | u32 `hue_mode` +0, `manual_hue.hue_sat_en` u16 +4 (hue dependent saturation table, 198 bytes), `hue_num` +202 (3..10), 10 auto entries from +204 stride 200 | symmetric | no | MEDIUM (a hue-selective saturation LUT: could pull the reds down; not exposed in the hook) |
| nr1 (raw / Bayer NR) | `ak_vpss_get_nr1_attr` 0x1bf9c / `set` 0x1bb2c | `(dev, void *)` 3064 bytes | u32 `nr1_mode` +0 (0..1); 17 records of 180 bytes from +4 (record 0 manual, 1..16 gain linked): `nr1_enable` +0, **`nr1_k` +2 (0..15)**, `nr_keepth` +4 (0..31), `nr1_lc_lut_key[16]`, `nr1_lc_lut[16]`, `nr1_weight_r/g/btbl[17]` | symmetric | no | HIGH names and bounds; MEDIUM that a larger k is stronger |
| nr2 (Y 2D NR) | `ak_vpss_get_nr2_attr` 0x1c5b8 / `set` 0x1c078 | `(dev, void *)` 990 bytes | u32 `nr2_mode` +0 (0..1); 17 records of 58 bytes from +4: `nr2_enable` +0, `nr2_pass2_enable` +2, **`nr2_k` +4 (0..15)**, `ynr_keep_th` +6 (0..31), **`ynr_intensity1` +8, `ynr_intensity2` +10 (0..15)**, `nr2_weight_tbl[17]` from +14, dpc fields at +48..+56 | symmetric | no | HIGH layout; MEDIUM on which field is "strength" |
| uvnr (chroma NR) | `ak_vpss_get_uvnr_attr` 0x1c958 / `set` 0x1c694 | `(dev, void *)` 718 bytes | u32 `uvnr_mode` +0; 17 records of 42 bytes from +4: `uvnr_enable` +0, `uvnr_pass2_enable` +2, **`uvnr_k` +4 (0..15)**, `uvnr_weight_r/g/btbl[]` | symmetric | no | HIGH |
| 3D NR (temporal) | `ak_vpss_get_3d_nr_attr` 0x1704c / `set` 0x17c50 | `(dev, void *)` 7076 bytes | u32 mode +0 (0..1); 17 records of 416 bytes from +4 (manual first, then 16 linked): **`tnr_enable` u16 +0 (0..1)**, `tnr_refFrame_format` +2 (0..1), further tnr thresholds and a 129-entry u16 table (<=15), `ynr_k`, `ynr_weight_tbl[17]`, `uvnr_k`, `sharp_k_th`, `sharp_factor_slop`, `sharp_factor_max` (offsets inside the record not decoded, LOW) | symmetric | no | HIGH for enable and the 416-byte record; LOW for the strengths |
| 3D NR reference compression | `ak_vpss_set_3dnr_rc` 0x17f40 / `ak_vpss_get_3dnr_rc_all` 0x17e64 | | `tnr_ref_rc_enable` | | no | LOW, leave alone |
| sharpness | effect type 4 (above); attribute `ak_vpss_get_sharp_attr` 0x1dfec / `set` 0x1ca34, `ak_vpss_set_sharp_reduce_factor` 0x1e9b4 (1536-byte struct) | | `check_vpss_sharp_attr`, `check_vpss_manual_sharp_attr`: manual + linked records | symmetric | type 4 | MEDIUM (effect), LOW (attribute layout) |
| Y gamma | `ak_vpss_get_y_gamma_attr` 0x1aa84 / `set` 0x1a800 | `(dev, void *)` | `ygamma[129]` u16 from +0, `ygamma_key[16]` (<=128) at +258, `ygamma_uv_adjust_enable/level`, `ygamma_cnoise_*` at +290..+304 | symmetric | no | MEDIUM |
| RGB gamma | `ak_vpss_get_rgb_gamma_attr` 0x19ae4 / `set` 0x19824 | `(dev, void *)` | `gain_threshold`, 16 linked `r/g/b_gamma`, `r/g/b_key` tables | symmetric | no | LOW layout |
| contrast attribute | `ak_vpss_get_contrast_attr` 0x1a228 / `set` 0x1a00c | `(dev, void *)` | u32 `cc_mode` +0, `y_contrast` u16 +4, `y_shift` s16 +6, then 16 linked records (`dark_pixel_area`, `dark_pixel_rate`, `shift_max`) | symmetric | type 3 | MEDIUM |
| WDR | effect type 5; `ak_vpss_get_wdr_attr` / `set` 0x15fd8 / 0x16720, 0x4228-byte struct, `check_wdr_params` | | the stock's wdr_type 2 path is get-modify-set of it | symmetric | 5 (40 / 0) | HIGH for type 5: **any -50..50, a linear blend of the conf curves with the identity ramp**, see the last section; the attr is documented in stock-firmware-reference.md |
| exposure | `ak_vpss_get_ae_attr` 0x14d40 / `set` 0x14e7c (1824-byte struct, `envi_gain_range`, `blacklight_compensation_en`, `exp_route_para.ae_route_node[]`), `ak_vpss_get_me_attr` / `set` (manual exposure: `a_gain`, `d_gain`, `isp_d_gain`), `ak_vpss_set_exp_type`, `ak_vpss_set_ae_convergence_rate` | | | symmetric | no | MEDIUM (names from the lib's checks, layouts not decoded). Out of scope for the colour question |

### 3.1 The effect types, from the code of `isp_set_effect` (0xe25c)

`effect_set` validates `value + 50 <= 100`, then edits the ISP's 10-byte YUV effect struct (or the sharpness / brightness structs) **relative to the
value the ISP had when the type was first used** (the base is read once per type and cached in a per-device table; `get` returns the last value set):

- **type 2 (saturation, HIGH on the field, MEDIUM on the name):** `uv_a = clamp(base_uv_a + 3 * value, 0, 255)`. 0 = the conf's saturation; the
  base value of `uv_a` is shown by the hook's `get yuv` (the usual unity is 64, so -21 would be about "grey" and +50 about 3.3x: a steep scale, use small steps).
- **type 3 (contrast):** `y_a += value` and `y_b -= 2 * value` (both clamped), HIGH.
- **type 0:** adds `value` to `uv_b` (a chroma offset: a tint shift, not a hue rotation in the usual sense). The Anyka header calls it hue; here it is a UV bias. MEDIUM. A **positive/negative V bias is a direct red/cyan control**: worth one test.
- type 1 brightness and 4 sharpness: other structs (`AK_ISP_CONTRAST`/`SHARP` scaled), as the stock uses them.
- The first call of a type reads the attribute, so the base is the conf value even if another tool changed it before. Setting `0` again returns to that base.

### 3.2 Flip and mirror: `ak_vpss_switch_flip_mirror` (HIGH on the call and its arguments, UNVERIFIED on the camera)

`int ak_vpss_switch_flip_mirror(int dev, int flip_en, int mirror_en)` (libplat_vpss 0x114f4; `ak_vpss_get_flip_mirror(dev, int *flip, int *mirror)` 0x1164c).
`ak_vpss_effect_set` type 8 ("flip bits") is NOT the way: the stock does not use it.

How it was confirmed (no header ships with the SDK; every statement below is read from the libraries and from the stock `yy_cam`):

- the lib's own error text, `param is error! flip_en:%d, mirror_en:%d`, names the two arguments: argument 2 is the flip, argument 3 the mirror; `dev` is 0..3
  (above that the lib logs and refuses), each flag must be 0 or 1 (anything else: refused, nothing written);
- the code: the lib keeps, per device, the orientation the sensor driver declared when its attributes were loaded (`isp_set_attr` copies the two u16 flip / mirror
  fields of the sensor attributes into a table that starts at -1 = "not loaded", the call fails while it is -1). The new orientation is that declared one **XOR**
  the argument: `0` = the sensor's native picture (what the app has always shown), `1` = reversed. It then calls `isp_set_flip_mirror`, which stores the pair
  and sends ISP ioctl `0x40044979` (the `FLIP_MIRROR` of `tools/hw_tests/parse_stock_capture.py`): the change is applied to the running ISP, **no stream restart**;
- the stock `yy_cam` (`yy_media_av130_isp_get_video_flip_mode` 0x2a19e4, called by `yy_media_av130_video_init` and by the Tuya "flip" data point) builds the two
  arguments from its `[dsp]` `flip_mode` (0 = none, 1 and 2 = one axis, 3 = both: `flip_mode` bit toggles an argument) combined with a per-sensor base field
  (`sensor1_flip_mode`) and calls the lib with them; it does not replay the call after a day/night switch.

Akam passes `(0, flip, mirror)` with the user's two switches (default 0, 0), so an unflipped camera behaves exactly as before: with both at 0 the app **never
calls** the function. Which of the two flags is vertical and which horizontal comes from the names of the arguments (flip = vertical, mirror = horizontal, the
usual meaning) and is **UNVERIFIED on the camera** (first thing to check: `image.flip=1` turns the picture upside down, `image.mirror=1` swaps left and right).
Also to check on the camera: that the lib's declared base orientation is the picture the camera shows today (so that 0 / 0 is the unchanged picture), that the
picture colour is right after a flip (the sensor Bayer order changes; the ISP is expected to follow), and that a day/night switch keeps the flip (the app replays
it after every `ak_vi_switch_mode`; the call is absolute, replaying it is harmless).

Where it lives in Akam: `a2k_isp_flip_mirror` (cap.c, the only caller), the state and every mapping in `platforms/anyka-ak3918av130/orient.[ch]`, the settings
rows `image.flip`, `image.mirror`, `image.ceiling` and the Home Assistant switches (control-api.md section 4, [MQTT](../../mqtt.md#lsc-rotatable-2k)). How each part of the app
follows the orientation is in section 3.3.

### 3.3 What follows the orientation (one convention for the whole app)

Two frames: the **displayed frame** (what the streams and the snapshots show: the detector, the tracker and everything a user types or reads is in it) and the
**sensor frame** (the picture with flip and mirror off). Anything stored that must outlive a change is kept in the sensor frame. The two mappings are involutions.

| Part | Frame / rule |
|---|---|
| Detection zones and ignore rectangles | **Stored in the sensor frame** (`akam/zones` on the card, `/config/akam/zones`), converted to the displayed frame by the detection (`a2k_zones_orient`) and by the Home Assistant text entities (they show and accept the displayed frame). A zone keeps covering the same part of the scene when the orientation changes: mirror: x1,x2 become 100-x2,100-x1; flip: the same for y. A card file is read as the sensor frame. |
| Person boxes (`detect.boxes`, the `detections` event), snapshots | Displayed frame (what the picture shows). |
| Person tracking | Works on the displayed frame; its motor sign is the measured one (invert_pan 1, invert_tilt 0 with the orientation off) with the pan reversed by the mirror and the tilt by the flip (`a2k_orient_invert_*`, applied live through a listener). |
| ONVIF ContinuousMove, `ptz.vel`, MQTT `ptz_vel`, the step buttons (`ptz.step`, MQTT `ptz_left` ...) | The viewer's directions: right / up are right / up in the picture. The driver speaks sensor directions: a mirror reverses left / right, a flip reverses up / down (`a2k_ptz_vel_manual`, `a2k_ptz_command`). |
| `ptz.move` (motor steps), presets, patrol routes, soft limits, the counters | Motor state: never depends on the orientation. |
| Scene reference (`/config/akam/ptz.scene`), the drift reference (RAM) and the FLAT check | The live detection frame is converted to the **sensor frame** before it is compared, saved or matched (`a2k_scene_feed`), so every reference stays comparable and **no reference is invalidated** by an orientation change. Frames of the first 1.5 s after a change (queued frames of the old orientation) are skipped. The drift sign constants (`A2K_PTZ_DRIFT_*_SIGN`) are motor facts and stay. |

Behaviour of a change (one rule for every consumer): the orientation is recorded first, then the ISP is told (the old one is restored if the ISP refuses). A **settling window** of 1.5 s opens: during it the scene check, the person tracker and the zone detector get no frame (the queued frames are of the old orientation); the tracker also forgets its target (its memory holds positions built with the old signs). The same window opens around the flip replay after a day / night switch, because the vendor lib may reset the flip inside `ak_vi_switch_mode` (not verified on the camera). A change of `image.ceiling` alone is saved (it writes `image.flip` and `image.mirror` to the settings file; the key itself is never in the file), and every change, from any source, republishes the Home Assistant switches and zone texts.

## 4. White balance and the "too red" question: what is known

- The stock never touches WB (section 1). With the ISP's AWB running (`wb_type` = the value read at start) the day set uses the 4-point CCM of section 2.
- `ak_vpss_set_soft_ps_param` (our `a2k_isp_dn_init`) only configures the day/night decision; its `gain_r` / `gain_b` fields are thresholds for the "non infrared" test, not WB controls.
- To **fix** a cast without losing the AWB there are three levers, in order of risk: (1) `fx 2` negative (saturation), (2) `ccm blend` toward the identity (keeps the white point, softens the strong off-diagonals that amplify any red bias), (3) `wb manual` with a lower R gain (loses the AWB: only for a fixed lighting).
  A permanent fix would be a get/modify/set of `ak_vpss_set_ccm_attr` after `ak_vi_switch_mode` (the mode switch reloads the whole set, so it has to be replayed after every switch, exactly like the WDR replay), or a patched copy of the conf file loaded with `ak_vi_load_sensor_cfg`; **not implemented, nothing decided before the measurements**.

### 4.1 `img.wb manual` is refused on the 2K (2026-11-24)

Hardware result: `img.wb manual` (wb_type switched to "the other value than the one read at start", the conf's own manual gains left as they are) gave a strong
blue cast at once; `conf` restored the picture. The user expectation is "manual = keep the current white balance", i.e. freeze the gains the AWB has now.
Evidence on whether that is possible with the vendor API (no camera access for this change, so read from the libraries and the earlier dumps only):

- `ak_vpss_get_mwb_attr` / `set_mwb_attr` read and write the **manual** gains (12 bytes: r/g/b gain u16 0..4095, offsets s16). Those hold the conf file's own manual
  values, not the gains the AWB runs with, so reading them does not give the current white balance. Their unit (256 = 1.0x?) is still UNVERIFIED (section 7).
- The only place that shows what the AWB currently uses is the 38 byte run info at +9376 of `ak_vpss_get_awb_stat_info`: u16 fields that are unnamed and whose
  meaning (gains, colour temperature) was never decoded (confidence LOW). Writing guessed fields as gains is exactly the kind of blind write that already gave the
  blue picture.
- `ak_vpss_get_awb_attr` is the AWB tuning (weights, steps, colour temperature table), not the current result.

So there is no safe way to read the current AWB gains and set them as the manual ones. Decision: `img.wb` accepts only `conf` on the 2K (the setting row lists
`conf` alone, `manual` is refused with HTTP 422 `range`, the assistant says why, nothing is sent to the ISP, `a2k_pic_set_wb(MANUAL)` returns `A2K_PIC_REFUSED`).
To enable it later: read `get awb` and `get wb` on the camera in the same lighting, decode the run info (compare it with the AWB gains of the picture), and
verify the gain unit with `wb manual R G B`; then freeze = write those gains and switch the type. The hook (`--isp-test 1`, `wb manual R G B`) is unchanged for that work.

## 5. Noise: what the controls are, and what they probably are not

The ISP has three denoisers in the chain, all with a manual record and 16 gain-linked records (the ISP picks / interpolates the linked one by the current sensor gain, which is why shadows (high gain) are noisier): **nr1** (Bayer domain), **nr2** (luma 2D) with its intensities, **uvnr** (chroma), plus the temporal **3D NR** (`tnr_enable`). The hook edits `nr1_k`, `nr2_k` (+ intensities), `uvnr_k` and `tnr_enable` in **all 17 records**, mode untouched, so the change is felt whatever the gain.
Blocky shadows may also be the **encoder** and not the ISP: the main stream runs VBR with a QP window (sub 32..50) and the 2K frame at 15 fps; in dark areas a high max QP makes blocks. Test the two causes apart (section 8, step 7): ISP noise shows as grain that a lower `nr2` / `3D NR` setting changes; blocking shows as flat squares and changes with `--venc-test` (`main qp 20 40`) and the bit rate.
Direction of `k` (bigger = more smoothing?) is MEDIUM; the test plan measures it.

## 6. IR-cut, the prime suspect (read of the code, HIGH on the code, UNVERIFIED on the camera)

- `ircut.c` does exactly the stock sequence: day = pin 24 high, 25 low, 300 ms, 24 low; night = 24 low, 25 high, 300 ms, 25 low; at start a day pulse. The stock does at init: mode 1 (day) pulse, mode 0 (night) pulse, then settles on day (3 pulses). We do one. A coil that is bistable moves on any pulse of the right polarity, so one is enough **if the polarity is right**.
- The only hardware evidence of the mapping is indirect (archive/rot2k-own-app-feasibility.md, 2026-11-15): forced night gave a bright mono IR frame (filter out), forced day gave a dark colour frame (sat 6). That proves the **night pulse moves the filter out**; that the **day pulse brings it fully back in** is only assumed (MEDIUM, "unverified until seen by eye").
- Signature of a filter that is not in the path in day light: the picture is pink / red with weak saturation structure, foliage and fabrics bright, black plastics purple, and the **R average clearly above G** (see `get rgb`). The cheap proof: **a TV remote pointed at the lens**. With the filter out (night position) the remote's IR LED shows as a bright white / violet spot in the live view; with the filter in it is invisible (or a faint dot). Do it in forced day (`night=day`) and then in forced night as the reference.
- Also compare the daylight `get rgb` ratios of forced day against the same scene seen by the stock (a stock frame).

## 7. The test hook (`--isp-test 1`, default OFF, RAM only)

Files: `platforms/anyka-ak3918av130/isptest.c` and `isptest.h`, vendor access in `cap.c` (`g_it_ops`), prototypes in `platforms/anyka-ak3918av130/abi.h`, host test `tests/rot2k_isptest_unit.c` (run by `tests/rot2k_overlay_test.sh`).
The hook is a poll thread (every 500 ms) reading `/tmp/akam2k_isp_test`; the file is deleted once read; up to 16 lines; every result is logged as `isp-test: ...` in the app log (before / after values, return codes). Nothing is persistent: the ISP restarts from the conf file at the next boot, `restore` puts back the first read of everything changed (effects are set back to 0). Option `--isp-test 1` is a command-line option of the binary only (no `akam.conf` key: run the binary from RAM like the `--venc-test` runs).

Commands (parser limits are enforced; `fx` type 5 / WDR is refused because the app owns it):

```
get all | fx | yuv | wb | ccm | nr1 | nr2 | uvnr | nr3d | awb | rgb | wdr   read only (nr2: enable, k, both intensities of the 17 records; wdr: head + sum of the 0x4228 bytes; fx: the cached effect 5 = isp.wdr - 50)
sat V | hue V | bright V | contrast V | sharp V | fx TYPE V             V -50..50, 0 = conf value
wb auto | wb manual R G B | wb type 0|1                                  gains 0..4095
ccm mode 0|1 | ccm blend P                                               P 0..100 percent of the factory matrices
nr1 K | nr2d K [I1 I2] | uvnr K | nr3d 0|1                              K 0..15
rt yuv|wb|ccm|nr1|nr2|uvnr|nr3d                                          get, set the same bytes back, get, compare
restore
```

## 8. Hardware test plan (coordinator, RAM only, step by step)

Setup: the camera in a lit room (daylight from a window preferred), the app started from RAM with its usual flags **plus `--isp-test 1`**, `night=day` (forced day) in the conf for steps 1 to 8. Commands from a shell on the camera: `echo "get all" > /tmp/akam2k_isp_test`, then read the new `isp-test:` lines of the log. For every picture step take one frame of stream0 (`ffmpeg -rtsp_transport tcp -i rtsp://<ip>:554/stream0 -frames:v 1 f.png`, keep it under `test-images/rot2k/`) and the numbers `ffmpeg -i f.png -vf signalstats -f null -` (SATAVG, UAVG, VAVG) next to the hook's `get rgb` line (R, G, B averages; a neutral scene gives R ~ G ~ B, a red cast gives R well above G).
Abort criteria: any `ak_*` rc other than 0 that repeats, a stream stall, the app exiting. The fall-back is always `restore`, then a reboot (the ISP returns to the conf file).

1. **Baseline, no change.** `get all`. Keep the whole log block. Check: `ccm mode 1` (automatic), 4 matrices (3000 / 4200 / 5200 / 6600 K, as in section 2: if the live values differ the layout or the section is not what is assumed: stop and report), `wb_type` value, the `mwb` gains (this shows the gain unit), `nr*` modes and `k` rows, `yuv_effect` (`uv_a` base), `awb run info`, `rgb average`. Frame + signalstats + `get rgb` = **day baseline**.
2. **IR-cut by eye (no hook).** Point a TV remote at the lens, press a key, look at the live view in `night=day`: a bright white / violet spot means the filter is **out** in day (cause found: pins, polarity or pulse). Repeat in forced night: the spot must show (reference). If the spot shows in day, also swap `A2K_IRCUT_DAY_PIN` / `A2K_IRCUT_NIGHT_PIN` in `ircut.h` and repeat (that is the one-line fix the own-app doc already names), then **stop here: the remaining steps are for a correct filter**.
3. **Round trips (no change of values).** `rt yuv`, `rt wb`, `rt ccm`, `rt nr1`, `rt nr2`, `rt uvnr`, `rt nr3d`, one per file/line. Expect `identical` and `set ... = 0`. A `DIFFERS` or a non-zero set means the layout or the set path is wrong for that attribute: do not use its commands, report the bytes.
4. **Saturation and tint.** `sat -10`, frame; `sat -20`, frame; `sat 10`, frame; `sat 0`. Expect SATAVG to fall / rise monotonically and `uv_a` in `get yuv` to move by 3 per step (in the log line `effect_get before/after`). Then `hue 5`, frame, `hue -5`, frame, `hue 0`: expect the average R/G/B to shift (one sign more red). Record the best value if the red is pulled out.
5. **White balance.** `get wb` and `get awb`; `wb manual R G B` starting from the conf's manual gains (the `mwb` line of step 1; if they are about 256 each the unit is 1.0x = 256) and from the gains in the AWB run info; then lower R by 10 % and 20 % one at a time (`wb manual 230 256 ...`), frame each; `wb auto` back, frame. Expect the manual picture to stop following the light (move a lamp). If `wb auto` does not bring back a picture identical to the baseline within a few seconds the auto type value was wrong: `wb type 0` and `wb type 1` both, report which is which (the hook labels the value read at start as auto).
6. **Colour matrix.** `ccm blend 70`, frame; `ccm blend 40`, frame; `ccm blend 0` (white-point-preserving unit matrix: saturation of the matrix removed), frame; `restore`. Expect a duller but less red picture as the blend decreases. `ccm mode 0` (the manual matrix 512 -200 -56 ...) only as a comparison.
7. **Noise and blocks.** Pick a shadow area (cover half of the window / a dark corner, gain high), frames at full size. `nr2d 0`, `nr2d 8`, `nr2d 15` (then `nr2d 8 8 8`), `uvnr 0` / `uvnr 15`, `nr1 0` / `nr1 15`, `nr3d 0` / `nr3d 1`, each followed by a frame and the bit rate over 10 s (the stream's kbit/s: a stronger NR lowers it). Note the direction of K per attribute and whether the grain or the blocks change. Then, with `--venc-test 1` also on, `main qp 20 40` / `main qp 32 50` to separate encoder blocking from ISP noise. `restore` between families.
8. **Combine.** The best saturation / blend / NR values together, one frame, compare with the stock frame of the same scene. `restore` and report the table (command, rc, R/G/B average, SATAVG, bit rate).
9. **Night check (after the day steps).** `night=night`: `get all`, confirm `ccm mode 0` and the luma matrix (the night set loaded by `ak_vi_switch_mode(0, 1)`), and that `nr*` differ from the day ones (the night set has its own tables). Nothing to change in night.

What to report: the first `get all` block (it settles the unit and layout questions), the `rt` results, the numbers per step and the frames. If steps 4 to 6 find a good setting, the follow-up is a persisted conf key replayed after each mode switch (a2k_image / a2k_isp_mode), the same way WDR is replayed; that design is **not done** and depends on those numbers.

## 8b. What the app does with this (2026-11-23, host-tested only)

`platforms/anyka-ak3918av130/picture.c` applies three of the controls above as settings, with the 1080p's names and ranges: `isp.saturation` (effect type 2, -50..50), `isp.denoise` (0..4: the 3D NR attribute
read whole, each gain-linked record i replaced by record i+N, the last stays, `tnr_enable` never lowered, written whole; refused without a write when the mode word or a record flag is not 0/1) and `img.wb`
(`conf` = untouched; `manual` = `ak_vpss_set_wb_type` to the other value than the one read before the first change, the conf's own manual gains unchanged, day set only). `ak_vi_switch_mode` reloads the set, so
`a2k_isp_mode` replays them after every switch (`a2k_pic_replay`). Not on a camera yet: the open questions below (which `wb_type` is automatic, the gain unit, the meaning of the 3D NR record) are exactly what the checklist P-items answer.

## 9. Open questions

- Which `wb_type` value is automatic (hook assumption: the one read at start). Gain unit of `r/g/b_gain` (256 = 1.0x?). Meaning of the 19 u16 of the AWB run info.
- Base value and unit of `uv_a` (the saturation effect is relative to it); whether type 0 is U or V or both.
- Which NR field is the best "strength" (k, intensity1/2, keep threshold), whether a larger value means more smoothing, and the inner offsets of the 3D NR record (only `tnr_enable` is addressed).
- Whether the conf file's night and day sets differ in NR / sharpness (step 9 answers it).
- `ak_vpss_get_rgb_average` argument order (MEDIUM): if the three numbers look wrong (all equal, or implausible) the order is different; the statistics block itself is readable with `get awb`.

## WDR strength (`isp.wdr`) and spatial noise reduction (`isp.nr2`), added 2026-11-24

Why: on a backlit scene (a bright window) the dark areas of the 2K are grainy and `isp.denoise` (temporal, 3D NR) changes almost nothing there.

**Effect type 5 is a continuous strength, not an on/off of 0 and 40 (HIGH, disassembly of `libplat_vpss.so`).**
- `ak_vpss_effect_set` (0x8a14) accepts the types 0..5 for `value + 50 <= 100`, i.e. **-50..50** (any other value is logged and refused with the lib's error). `isp_set_effect` (0xe25c) dispatches type 5 to a static function at 0xccd8 that is the whole WDR effect.
- That function computes `w = (value + 50) * 1024 / 100`, takes the ISP's cached copy of the conf file's WDR attribute (the first call reads it with the get path, later calls start from that copy again: **never cumulative**) and, for six 65-entry u16 curves of the first group and then the same for the following groups of the attribute (10-bit values, clamped to 1023), writes `(w * conf + (1024 - w) * default) / 1024`, where `default` is `default_wdr_para` (rodata 0x22664): the straight ramp 0, 16, 32 .. 1008, 1023. The blended attribute goes through `ak_vpss_set_wdr_attr` (so `check_wdr_params` validates the result: blends of two valid curves stay valid). The value is cached for `ak_vpss_effect_get`.
- So: **-50 = the identity ramp (WDR off), 50 = the conf file's curves in full, linear in between.** The stock's 40 is 90 % of the conf curves; the stock's "off" call (0) is in fact 50 % and not off. `isp.wdr` 0..100 is `value + 50`: 0 = off, 90 = the stock (default, effect 40), 100 = effect 50.
- No get-modify-set of the 0x4228-byte attribute is needed or done (the stock's `wdr_type 2` path does one; not used). The attribute is only read by the `--isp-test` hook (`get wdr`).
- Uncertain without a camera: that values 41..50 look stronger than the stock (the maths says so); that the cached conf copy is the day set only (the stock has the same limit); which of the curves the sensor path actually uses.
- Replay: the vendor's mode switch resets it. The app's replay after a switch (`a2k_isp_wdr(1)`, 100 ms after the switch, unchanged timing) now applies the user's value (40 when `isp.wdr` is untouched, so nothing changes by default); `a2k_pic_replay` also re-applies it once the user changed it, which covers `--wdr 0`.

**`isp.nr2` (`ak_vpss_get_nr2_attr` / `set`, 990 bytes, layout in the table above).** Get-modify-set from the pristine table captured at the first change (and again after every mode switch): for the manual record `nr2_k`, `ynr_intensity1` and `ynr_intensity2` get `+N`, for the gain-linked record i (1..16, low gain first) `+(N * (i + 4) + 4) / 8` (from about N * 5/8 at low gain to N * 20/8 at the highest), clamped at 15. Mode word, enables, `ynr_keep_th`, weights and DPC fields are untouched. The attribute is refused, nothing sent, when the mode word is above 1, an enable is above 1, `nr2_k` or an intensity is above 15 or `ynr_keep_th` is above 31. N = 0 sends nothing on an untouched camera and writes the original table back after a change.
- **Unproved:** that a larger `nr2_k` / `ynr_intensity` is a stronger filter (the vendor naming says so; the hook's `nr2d` command exists to find the direction), and that the records of the high-gain end are enabled (`nr2_enable` is not touched: a record with the filter off stays off). The hardware run reads `get nr2` before and after (PIC-12, PIC-13).
