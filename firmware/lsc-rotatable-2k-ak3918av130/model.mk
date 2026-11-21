# Model description of the LSC Rotatable 2K (AK3918AV130), included by platforms/anyka-ak3918av130/Makefile (see its header).
# Holds what is specific to this camera: the features, the ORDERED object list, the model-only compile flags, where the vendor libraries and the
# published binaries live, and the helper programs with their build rules.

# Where `make poc3-dist` publishes a copy of the binaries and BUILDINFO.txt (what build_card.sh, build_appfs.sh and make_kit_2k.sh read) and where
# toolchain/prepare_vendor.sh puts the vendor libraries of YOUR backup (link-time only). Build outputs themselves are in build/$(MODEL).
PUBLISH_DIR := $(ROOT)/dist/rot2k-app
VENDOR      := $(PUBLISH_DIR)/vendor/appfs/lib
# Model-only compile flags: sizes and file names tuned for this camera (not platform facts). Extra include directories of the model.
MODEL_CFLAGS := -DA2K_API_RESP_MAX=16384 -DONVIF_SCRATCH=2048 -DWSD_INSTANCE_FILE='"/tmp/akam2k_wsd_instance"' -DA2K_API_FAST_ACTION='"ptz.vel"' \
                -DAPISIG_NONCE_CAP=224 -DCRED_NONCE_CACHE=16
MODEL_INC_DIRS := $(MODEL_DIR)/helpers/ptz

# Features of this model (src/build/features.mk). All on today: moving to feat_objs is not a behaviour change.
AKAM_FEAT_API    := 1
AKAM_FEAT_PTZ    := 1
AKAM_FEAT_BUTTON := 1
AKAM_FEAT_SCENE  := 1
AKAM_FEAT_PATROL := 1
AKAM_FEAT_TRACK  := 1
AKAM_FEAT_SYS    := 1
AKAM_FEAT_LIGHT  := 1
AKAM_FEAT_ONVIF  := 1
SHARED_AK := h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtp_clock tx_pace net_tx md5 mqtt mqtt_ha night_logic cfg_file
# PTZ, TRACK, SCENE and PATROL gate their objects below (a 0 really drops ptz/hal_ptz, track, scene, patrol and their shared modules), but the model code
# that stays (settings.c, detect.c, main.c, mqtt_bind.c, the API binding ...) still calls them without an `ifdef`, so a build with one of them off does not
# link (measured: SCENE 5 undefined symbols, PATROL 19, TRACK 43, PTZ 50). That needs a stub layer per feature, to be written together with the split of this
# directory (docs/hacking.md, "the 2K model directory"). Until then the switch is refused instead of failing at the linker;
# AKAM_ALLOW_UNLINKABLE=1 lifts the refusal (to list the symbols a stub layer has to provide).
$(foreach f,PTZ TRACK SCENE PATROL,$(if $(filter 1,$(AKAM_FEAT_$(f))),,$(if $(AKAM_ALLOW_UNLINKABLE),,$(error AKAM_FEAT_$(f) := 0 does not link on this model yet: the model code calls $(f) unconditionally, see the comment in $(MODEL_DIR)/model.mk))))

# Link order = the pre-move order of the objects (section placement of the stripped binary follows it). Never sort it, never use a wildcard.
# `c:` names a shared module of src/ (src/build/sources.mk; feature-gated when wrapped in feat_objs), the other names are sources of this
# directory (firmware/<model>) or of the platform directory (platforms/anyka-ak3918av130), found by the pattern rules of the platform Makefile.
# A model without a feature links none of its shared objects.
POC3_ORDER = $(call feat_objs,PATROL,c:ptz_patrol patrol patrol_run) main aring audio cap stream rtsp_session detect detstate mqtt_bind mqtt_cfg zones snap pin ircut image night_ctl night_run \
  $(call feat_objs,SYS,c:hal_sys) \
  leds leds_run hal hal_light_sysfs \
  $(call feat_objs,LIGHT,c:light_logic c:led_pat) \
  $(call feat_objs,PTZ,hal_ptz hal_ptz_drv ptz ptz_desc) $(call feat_objs,SCENE,scene c:scene_check) $(call feat_objs,PTZ,c:drift_match) \
  $(call feat_objs,BUTTON,c:hal_input) \
  hal_input_sysfs \
  $(call feat_objs,BUTTON,c:button_logic) \
  button store $(call feat_objs,TRACK,track track_run) zlive $(call feat_objs,TRACK,c:track_logic c:track_relay) video_cfg video vlive isptest netreg \
  $(call feat_objs,API,c:api_events) \
  detpub settings api_bind $(call feat_objs,API,c:api_core c:api_httpd c:http_req) \
  $(call feat_objs,BUTTON,c:button_gesture c:button_cfg) \
  button_rt \
  $(call feat_objs,API,c:api_limit c:api_keys c:api_sig c:sha256 c:json_min c:settings_reg) \
  $(addprefix c:,$(SHARED_AK)) \
  $(call feat_objs,PTZ,c:ptz_vel c:ptz_lease) \
  $(call feat_objs,ONVIF,c:http_srv c:soap c:onvif c:wsd c:cred c:sha1 c:b64 onvif_services $(if $(and $(call feat_on,ONVIF),$(call feat_on,PTZ)),onvif_ptz) onvif_model)

# ---- helper programs of this model --------------------------------------------------------------------------------------------------------------
# HELPERS = key:binary:argument of image/source_hash.sh. The platform Makefile builds them in the symcheck/dist targets and records them in BUILDINFO
# (<key>_source_sha256, <key>_sha256, <key>_size). Each is plain libc, linked without the vendor libraries.
HELPERS := sntp:akam2k_sntp:sntp mic:akam2k_mic:audio led:akam2k_led:led
.PHONY: sntp mic led

# akam2k_sntp: the one-shot SNTP client that stands in for the missing BusyBox ntpd (helpers/sntp of the model, host-tested core). It uses
# plain libc only (sockets, getaddrinfo, settimeofday): no ctype, no vendor lib, no SmolRTSP.
SNTP_SRC := $(MODEL_DIR)/helpers/sntp/sntp.c $(MODEL_DIR)/helpers/sntp/sntp_core.c
sntp: $(OUT)/akam2k_sntp
$(OUT)/akam2k_sntp: $(SNTP_SRC) $(MODEL_DIR)/helpers/sntp/sntp_core.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I$(MODEL_DIR)/helpers/sntp -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(SNTP_SRC)

# akam2k_mic: the standalone microphone PoC (helpers/mic of the model, host-tested core). Plain libc only, no vendor lib, no libm. A manual
# tool: copied by hand to /tmp on the camera, never installed by build_card.sh.
MIC_SRC := $(MODEL_DIR)/helpers/mic/mic.c $(MODEL_DIR)/helpers/mic/mic_core.c
mic: $(OUT)/akam2k_mic
$(OUT)/akam2k_mic: $(MIC_SRC) $(MODEL_DIR)/helpers/mic/mic_core.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I$(MODEL_DIR)/helpers/mic -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(MIC_SRC)

# akam2k_led: the light PoC / control tool (hal_light: portable logic + generic sysfs driver + the 2K descriptor). Plain libc only,
# no vendor lib. A manual tool: copied by hand to /tmp on the camera, never installed by build_card.sh.
LED_SRC := $(MODEL_DIR)/helpers/led/led.c $(call common_src,light_logic) $(MODEL_DIR)/hal_light_sysfs.c $(MODEL_DIR)/hal.c $(MODEL_DIR)/pin.c
led: $(OUT)/akam2k_led
$(OUT)/akam2k_led: $(LED_SRC) $(SRC_DIR)/hal/hal_light.h $(SRC_DIR)/features/light/light_logic.h $(MODEL_DIR)/hal.h $(MODEL_DIR)/pin.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I. -I$(MODEL_DIR) -I$(MODEL_DIR)/helpers/ptz $(common_inc) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(LED_SRC)
