# Build-only skeleton of the Rotatable 2K own app (study, see docs/porting/lsc-rotatable-2k/own-app-feasibility.md).
# Not part of any release, CI build or card. Output goes to dist/ (git-ignored).
#
#   app/fetch_toolchain.sh                      # once: pinned uClibc toolchain into tools/.toolchain
#   app/prepare_vendor.sh ~/akam-backup-...     # once: vendor .so from YOUR backup into dist/rot2k-app/vendor
#   make -C firmware/lsc-rotatable-2k-ak3918av130/app poc0 poc2 poc4   # or: make all
#
# The binary is dynamically linked against the camera's own libs: run it with LD_LIBRARY_PATH=/appfs/lib.
ROOT    := $(abspath ../../..)
TC      := $(ROOT)/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1
CC      := $(TC)/bin/arm-linux-gcc
VENDOR  := $(ROOT)/dist/rot2k-app/vendor/appfs/lib
OUT     := $(ROOT)/dist/rot2k-app
CFLAGS  ?= -Os -Wall -Wextra -march=armv5te -mfloat-abi=soft
LDLIBS  := -L$(VENDOR) -lplat_vi -lplat_vpss -lplat_isp_sdk -lmpi_venc -lplat_mem -lplat_common -lplat_log \
           -lplat_osal -lplat_thread -lplat_dbg -lapp_video
# The vendor libs reference each other and the camera's libc: resolve those on the camera, not at link time.
LDFLAGS ?= -Wl,--allow-shlib-undefined -Wl,-rpath-link,$(VENDOR)

# NPU userland of the detection thread (see the poc4 rule).
NPULIBS := -lmpi_svp2 -lplat_npu -lak_nne

.PHONY: all poc0 poc2 poc3 sntp mic led poc3-symcheck poc3-dist poc4 clean
all: poc0 poc2 poc3 poc4
poc0: $(OUT)/poc0_versions
poc2: $(OUT)/poc2_frame
poc3: $(OUT)/akam2k_rtsp
poc4: $(OUT)/poc4_detect

$(OUT)/poc0_versions: poc0_versions.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# PoC-2 needs C11 (_Static_assert on the transcribed struct sizes); gnu11 is the compiler default, stated for clarity.
$(OUT)/poc2_frame: poc2_frame.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# PoC-4 (NPU detection) also links the NPU userland: libmpi_svp2 (ak_svp2_*), libplat_npu (ak_npu_*), libak_nne (NNE_*,
# needed at run time by libplat_npu) and libak_mrd (not used by us, listed by the stock). libakv_cnn_dt is not linked by
# the stock either (the detection post-processing runs inside the NPU kernel driver).
$(OUT)/poc4_detect: poc4_detect.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -lmpi_svp2 -lplat_npu -lak_nne

# ---- PoC-3: akam2k_rtsp -------------------------------------------------------------------------------------------
# The RTSP/RTP/SDP/H.264 sources are the SHARED ones, in src/common (listed once in src/common/common.mk), compiled
# UNMODIFIED from where they are (PLAN section 8ter, phase A). night_logic.h still includes night.h of src/ak_rtsp.
# SmolRTSP is a fetched dependency that is not tracked in git: point SMOLRTSP at a checkout that has build/_deps
# (the one the 1080p build uses), e.g. make poc3 SMOLRTSP=/path/to/src/smolrtsp
AKRTSP   := $(ROOT)/src/ak_rtsp
SMOLRTSP ?= $(ROOT)/src/smolrtsp
POC3_OBJ := $(OUT)/poc3
COMMON_DIR := $(ROOT)/src/common
include $(COMMON_DIR)/common.mk
# Features of this model (src/common/features.mk). All on today: moving to feat_objs is not a behaviour change.
AKAM_FEAT_API    := 1
AKAM_FEAT_PTZ    := 1
AKAM_FEAT_BUTTON := 1
AKAM_FEAT_SCENE  := 1
AKAM_FEAT_PATROL := 1
AKAM_FEAT_WIFI5G := 0
AKAM_FEAT_TRACK  := 1
AKAM_FEAT_SYS    := 1
AKAM_FEAT_LIGHT  := 1
include $(COMMON_DIR)/features.mk
.DEFAULT_GOAL := all
SHARED_AK := h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtp_clock tx_pace net_tx md5 mqtt mqtt_ha night_logic cfg_file
# Link order = the pre-move order of the objects (section placement of the stripped binary follows it: byte-identical across
# the moves). `c:` names a src/common object (feature-gated), the others are 2K app objects. A model without a feature
# links none of its common objects.
POC3_ORDER := ptz_patrol akam2k_patrol akam2k_patrol_run akam2k_main akam2k_aring akam2k_audio akam2k_cap akam2k_stream akam2k_rtsp akam2k_detect akam2k_detstate akam2k_mqtt akam2k_mqttcfg akam2k_zones akam2k_snap akam2k_pin akam2k_ircut akam2k_image akam2k_night akam2k_night_run \
  $(call feat_objs,SYS,c:hal_sys) \
  akam2k_leds akam2k_leds_run akam2k_hal hal_light_sysfs \
  $(call feat_objs,LIGHT,c:light_logic c:led_pat) \
  hal_ptz hal_ptz_drv akam2k_ptz akam2k_ptz_desc akam2k_scene scene_check \
  $(call feat_objs,BUTTON,c:hal_input) \
  hal_input_sysfs \
  $(call feat_objs,BUTTON,c:button_logic) \
  akam2k_button akam2k_store akam2k_track akam2k_track_run akam2k_zlive track_logic akam2k_vidcfg akam2k_video akam2k_vlive akam2k_netreg \
  $(call feat_objs,API,c:api_events) \
  akam2k_detpub akam2k_settings akam2k_apibind $(call feat_objs,API,c:api_core c:api_httpd c:http_req) \
  $(call feat_objs,BUTTON,c:button_gesture c:button_cfg) \
  akam2k_buttonrt \
  $(call feat_objs,API,c:api_limit c:api_keys c:api_sig c:sha256 c:json_min c:settings_reg) \
  $(addprefix c:,$(SHARED_AK))
poc3_obj = $(if $(filter c:%,$(1)),$(POC3_OBJ)/ak_$(patsubst c:%,%,$(1)).o,$(POC3_OBJ)/own_$(1).o)
SMOL_SRC := $(wildcard $(SMOLRTSP)/src/*.c) $(wildcard $(SMOLRTSP)/src/types/*.c) $(wildcard $(SMOLRTSP)/src/writer/*.c) \
            $(wildcard $(SMOLRTSP)/src/transport/*.c) $(SMOLRTSP)/src/nal/h264.c
POC3_CFLAGS := -std=gnu99 $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP -I. -I$(AKRTSP) $(common_inc) \
               -I$(SMOLRTSP)/include -I$(SMOLRTSP)/build/_deps/slice99-src -I$(SMOLRTSP)/build/_deps/datatype99-src \
               -I$(SMOLRTSP)/build/_deps/interface99-src -I$(SMOLRTSP)/build/_deps/metalang99-src/include \
               -DSMOLRTSP_WITH_H264
POC3_OBJS := $(foreach o,$(POC3_ORDER),$(call poc3_obj,$(o))) \
             $(addprefix $(POC3_OBJ)/smol_,$(subst /,_,$(patsubst $(SMOLRTSP)/src/%.c,%.o,$(SMOL_SRC))))
# (the smol_ object names flatten the sub directories, see the rules below)

# SmolRTSP's parser uses the ctype macros, which this toolchain's headers turn into __ctype_b_loc(), a symbol the
# camera's libc does not export (see akam2k_ctype.h). Only the SmolRTSP objects need it (rtsp_auth.c defines
# _GNU_SOURCE itself and must not see a ctype.h included before that); `make poc3-symcheck` catches any other user.
$(POC3_OBJ)/smol_%.o: POC3_CFLAGS += -include akam2k_ctype.h

define poc3_check
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(SMOLRTSP)/build/_deps" || { echo "smolrtsp checkout missing: pass SMOLRTSP=/path/to/src/smolrtsp"; exit 1; }
	@mkdir -p $(POC3_OBJ)
endef

$(POC3_OBJ)/own_%.o: %.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/ak_%.o: $(COMMON_DIR)/net/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/ak_%.o: $(COMMON_DIR)/logic/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/ak_%.o: $(COMMON_DIR)/api/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<

# One rule per SmolRTSP sub directory (object names carry the directory so that types/header.c and writer.c, ...
# cannot collide).
$(POC3_OBJ)/smol_%.o: $(SMOLRTSP)/src/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/smol_types_%.o: $(SMOLRTSP)/src/types/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/smol_writer_%.o: $(SMOLRTSP)/src/writer/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/smol_transport_%.o: $(SMOLRTSP)/src/transport/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/smol_nal_%.o: $(SMOLRTSP)/src/nal/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<

$(OUT)/akam2k_rtsp: $(POC3_OBJS)
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	$(CC) $(LDFLAGS) -Wl,--gc-sections -pthread -o $@ $^ $(LDLIBS) $(NPULIBS)

# akam2k_sntp: the one-shot SNTP client that stands in for the missing BusyBox ntpd (../sntp, host-tested core). It uses
# plain libc only (sockets, getaddrinfo, settimeofday): no ctype, no vendor lib, no SmolRTSP.
SNTP_SRC := ../sntp/akam2k_sntp.c ../sntp/akam2k_sntp_core.c
sntp: $(OUT)/akam2k_sntp
$(OUT)/akam2k_sntp: $(SNTP_SRC) ../sntp/akam2k_sntp_core.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I../sntp -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(SNTP_SRC)

# akam2k_mic: the standalone microphone PoC (../audio, host-tested core). Plain libc only, no vendor lib, no libm. A manual
# tool: copied by hand to /tmp on the camera, never installed by build_card.sh.
MIC_SRC := ../audio/akam2k_mic.c ../audio/akam2k_mic_core.c
mic: $(OUT)/akam2k_mic
$(OUT)/akam2k_mic: $(MIC_SRC) ../audio/akam2k_mic_core.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I../audio -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(MIC_SRC)

# akam2k_led: the light PoC / control tool (hal_light: portable logic + generic sysfs driver + the 2K descriptor). Plain libc only,
# no vendor lib. A manual tool: copied by hand to /tmp on the camera, never installed by build_card.sh.
LED_SRC := akam2k_led.c $(COMMON_DIR)/logic/light_logic.c hal_light_sysfs.c akam2k_hal.c akam2k_pin.c
led: $(OUT)/akam2k_led
$(OUT)/akam2k_led: $(LED_SRC) $(COMMON_DIR)/logic/hal_light.h $(COMMON_DIR)/logic/light_logic.h akam2k_hal.h akam2k_pin.h
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu99 $(CFLAGS) -I. $(common_inc) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(LED_SRC)

# Every libc symbol the binary needs must be exported by the CAMERA's libc, not only by the build toolchain's.
#   make poc3-symcheck CAMERA_LIBC=/path/to/libuClibc-1.0.31.so   (from your own camera backup)
CAMERA_LIBC ?=
poc3-symcheck: $(OUT)/akam2k_rtsp $(OUT)/akam2k_sntp $(OUT)/akam2k_mic $(OUT)/akam2k_led
	@test -f "$(CAMERA_LIBC)" || { echo "pass CAMERA_LIBC=<libuClibc-*.so taken from your own camera backup>"; exit 1; }
	@for b in akam2k_rtsp akam2k_sntp akam2k_mic akam2k_led; do $(TC)/bin/arm-linux-readelf --dyn-syms $(OUT)/$$b | awk '$$7=="UND"{print $$8}' | sed 's/@.*//' | grep -v -E '^(ak_|_ITM|__deregister_frame_info|__register_frame_info|$$)'; done | sort -u >$(OUT)/.und
	@$(TC)/bin/arm-linux-readelf --dyn-syms $(CAMERA_LIBC) | awk '$$7!="UND"{print $$8}' | sed 's/@.*//' | sort -u >$(OUT)/.exp
	@missing=$$(comm -23 $(OUT)/.und $(OUT)/.exp); if [ -n "$$missing" ]; then echo "NOT exported by the camera libc: $$missing"; exit 1; else echo "poc3-symcheck: all libc symbols are exported by the camera libc"; fi

# Card input for build_card.sh (--own-app-bin / dist/rot2k-app): runs the symcheck (so CAMERA_LIBC is required), strips
# the binary and writes BUILDINFO.txt next to it (sha256 of the stripped binary, hash of the sources, the proof that
# the symcheck passed and against which libc). build_card.sh refuses a binary without that proof or with a stale hash.
poc3-dist: poc3-symcheck
	@$(TC)/bin/arm-linux-strip $(OUT)/akam2k_rtsp $(OUT)/akam2k_sntp $(OUT)/akam2k_mic $(OUT)/akam2k_led
	@interp=$$($(TC)/bin/arm-linux-readelf -l $(OUT)/akam2k_rtsp | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p'); \
	test "$$interp" = /lib/ld-uClibc.so.0 || { echo "unexpected interpreter: $$interp"; exit 1; }; \
	sinterp=$$($(TC)/bin/arm-linux-readelf -l $(OUT)/akam2k_sntp | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p'); \
	test "$$sinterp" = /lib/ld-uClibc.so.0 || { echo "unexpected sntp interpreter: $$sinterp"; exit 1; }; \
	minterp=$$($(TC)/bin/arm-linux-readelf -l $(OUT)/akam2k_mic | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p'); \
	test "$$minterp" = /lib/ld-uClibc.so.0 || { echo "unexpected mic interpreter: $$minterp"; exit 1; }; \
	linterp=$$($(TC)/bin/arm-linux-readelf -l $(OUT)/akam2k_led | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p'); \
	test "$$linterp" = /lib/ld-uClibc.so.0 || { echo "unexpected led interpreter: $$linterp"; exit 1; }; \
	{ echo "tool=akam2k_rtsp"; \
	  echo "source_sha256=$$(sh ./source_hash.sh)"; \
	  echo "sha256=$$(sha256sum $(OUT)/akam2k_rtsp | cut -d' ' -f1)"; \
	  echo "size=$$(stat -c %s $(OUT)/akam2k_rtsp)"; \
	  echo "interpreter=$$interp"; \
	  echo "symcheck=passed"; \
	  echo "sntp_source_sha256=$$(sh ./source_hash.sh sntp)"; \
	  echo "sntp_sha256=$$(sha256sum $(OUT)/akam2k_sntp | cut -d' ' -f1)"; \
	  echo "sntp_size=$$(stat -c %s $(OUT)/akam2k_sntp)"; \
	  echo "mic_source_sha256=$$(sh ./source_hash.sh audio)"; \
	  echo "mic_sha256=$$(sha256sum $(OUT)/akam2k_mic | cut -d' ' -f1)"; \
	  echo "mic_size=$$(stat -c %s $(OUT)/akam2k_mic)"; \
	  echo "led_source_sha256=$$(sh ./source_hash.sh led)"; \
	  echo "led_sha256=$$(sha256sum $(OUT)/akam2k_led | cut -d' ' -f1)"; \
	  echo "led_size=$$(stat -c %s $(OUT)/akam2k_led)"; \
	  echo "camera_libc_sha256=$$(sha256sum $(CAMERA_LIBC) | cut -d' ' -f1)"; \
	} >$(OUT)/BUILDINFO.txt
	@echo "poc3-dist: $(OUT)/akam2k_rtsp + $(OUT)/akam2k_sntp + $(OUT)/akam2k_mic + $(OUT)/akam2k_led + BUILDINFO.txt"

-include $(POC3_OBJS:.o=.d)

clean:
	rm -f $(OUT)/poc0_versions $(OUT)/poc2_frame $(OUT)/akam2k_rtsp $(OUT)/akam2k_sntp $(OUT)/akam2k_mic $(OUT)/akam2k_led $(OUT)/poc4_detect
	rm -rf $(POC3_OBJ)
