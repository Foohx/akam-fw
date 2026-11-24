# Build of the Anyka AK3918AV130 platform (vendor libraries, dynamic uClibc, gcc 2018.11): the app of the LSC Rotatable 2K, shipped as akamd (`akam2k_rtsp` in 0.1.x)
# with its helpers akam-sntp, akam-mic, akam-led. Run from this directory, or through the root Makefile (`make 2k`).
# What differs per camera model lives in firmware/<MODEL>/model.mk (features, the ORDERED object list); the shared modules are named by
# src/build/sources.mk; the generic rules, flags and the dist/symcheck targets are here. Everything a build writes goes under
# build/$(MODEL) (git-ignored; MODEL=<id> selects the model, two models never overwrite each other). `poc3-dist` then PUBLISHES a copy of the
# stripped binaries and BUILDINFO.txt to the model's PUBLISH_DIR (model.mk; dist/rot2k-app for the 2K), the location build_card.sh,
# build_appfs.sh and tools/release/make_kit_2k.sh read.
#
#   toolchain/fetch_toolchain.sh                    # once: pinned uClibc toolchain into tools/.toolchain
#   toolchain/prepare_vendor.sh ~/akam-backup-...   # once: vendor .so from YOUR backup into dist/rot2k-app/vendor
#   make -C platforms/anyka-ak3918av130 poc3-dist CAMERA_LIBC=<libuClibc-*.so of your backup>   # or: make poc0 poc2 poc4 (probes/)
# What is specific to a camera model is NOT here: the model-only compile flags (MODEL_CFLAGS), the helper programs (HELPERS and their rules), the vendor
# library directory and the publish directory are in firmware/<MODEL>/model.mk.
#
# The binary is dynamically linked against the camera's own libs: run it with LD_LIBRARY_PATH=/appfs/lib.
ROOT    := $(abspath ../..)
MODEL   ?= lsc-rotatable-2k-ak3918av130
REL     := ../..
MODEL_DIR := $(REL)/firmware/$(MODEL)
SRC_DIR := $(REL)/src
TC      := $(ROOT)/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1
CC      := $(TC)/bin/arm-linux-gcc
OUT     := $(ROOT)/build/$(MODEL)
include $(SRC_DIR)/build/sources.mk
# The model: AKAM_FEAT_*, the shared module list and the ordered object list POC3_ORDER (src/build/features.mk gates the `c:` entries), MODEL_CFLAGS,
# VENDOR, PUBLISH_DIR and the helper programs.
include $(MODEL_DIR)/model.mk
include $(SRC_DIR)/build/features.mk
# A model C file must not share its basename with a C file of the platform directory: the platform file would be compiled and the model's ignored
# (or the reverse) without any error. Checked at parse time; tools/ci/layering_lint.sh and tests/layering_lint_test.sh cover it.
$(foreach f,$(notdir $(wildcard $(MODEL_DIR)/*.c)),$(if $(wildcard $(f)),$(error basename clash: $(f) exists in both the platform directory and $(MODEL_DIR))))

CFLAGS  ?= -Os -Wall -Wextra -march=armv5te -mfloat-abi=soft
LDLIBS  := -L$(VENDOR) -lplat_vi -lplat_vpss -lplat_isp_sdk -lmpi_venc -lplat_mem -lplat_common -lplat_log \
           -lplat_osal -lplat_thread -lplat_dbg -lapp_video
# The vendor libs reference each other and the camera's libc: resolve those on the camera, not at link time.
LDFLAGS ?= -Wl,--allow-shlib-undefined -Wl,-rpath-link,$(VENDOR)

# NPU userland of the detection thread (see the poc4 rule).
NPULIBS := -lmpi_svp2 -lplat_npu -lak_nne

.PHONY: all poc0 poc2 poc3 poc3-symcheck poc3-dist poc4 clean
all: poc0 poc2 poc3 poc4
poc0: $(OUT)/poc0_versions
poc2: $(OUT)/poc2_frame
poc3: $(OUT)/akamd
poc4: $(OUT)/poc4_detect

$(OUT)/poc0_versions: probes/poc0_versions.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# PoC-2 needs C11 (_Static_assert on the transcribed struct sizes); gnu11 is the compiler default, stated for clarity.
$(OUT)/poc2_frame: probes/poc2_frame.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# PoC-4 (NPU detection) also links the NPU userland: libmpi_svp2 (ak_svp2_*), libplat_npu (ak_npu_*), libak_nne (NNE_*,
# needed at run time by libplat_npu) and libak_mrd (not used by us, listed by the stock). libakv_cnn_dt is not linked by
# the stock either (the detection post-processing runs inside the NPU kernel driver).
$(OUT)/poc4_detect: probes/poc4_detect.c
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	@mkdir -p $(OUT)
	$(CC) -std=gnu11 $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS) -lmpi_svp2 -lplat_npu -lak_nne

# ---- PoC-3: akamd -------------------------------------------------------------------------------------------
# The RTSP/RTP/SDP/H.264 sources are the SHARED ones, in src/ (listed once in src/build/sources.mk), compiled
# UNMODIFIED from where they are (PLAN section 8ter, phase A). night_logic.h includes night.h, which lives in src/features/night too.
# SmolRTSP is a fetched dependency that is not tracked in git: point SMOLRTSP at a checkout that has build/_deps
# (the one the 1080p build uses), e.g. make poc3 SMOLRTSP=/path/to/third_party/smolrtsp
# Reproducibility: the shared and SmolRTSP sources are named RELATIVE to this directory, because their assert/__FILE__ strings
# end up in the binary and this toolchain's gcc 7 has no -ffile-prefix-map. A relative name makes the stripped binary independent
# of where the checkout lives (tools/ci/repro_check_rot2k.sh proves it). An absolute SMOLRTSP is converted (logically, symlinks
# are not resolved).
SMOLRTSP ?= $(REL)/third_party/smolrtsp
override SMOLRTSP := $(shell realpath -ms --relative-to=. "$(SMOLRTSP)")
POC3_OBJ := $(OUT)/poc3
.DEFAULT_GOAL := all
poc3_obj = $(if $(filter c:%,$(1)),$(POC3_OBJ)/ak_$(patsubst c:%,%,$(1)).o,$(POC3_OBJ)/own_$(1).o)
SMOL_SRC := $(wildcard $(SMOLRTSP)/src/*.c) $(wildcard $(SMOLRTSP)/src/types/*.c) $(wildcard $(SMOLRTSP)/src/writer/*.c) \
            $(wildcard $(SMOLRTSP)/src/transport/*.c) $(SMOLRTSP)/src/nal/h264.c
POC3_CFLAGS := -std=gnu99 $(MODEL_CFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -MMD -MP -I. -I$(MODEL_DIR) $(addprefix -I,$(MODEL_INC_DIRS)) $(common_inc) \
               -I$(SMOLRTSP)/include -I$(SMOLRTSP)/build/_deps/slice99-src -I$(SMOLRTSP)/build/_deps/datatype99-src \
               -I$(SMOLRTSP)/build/_deps/interface99-src -I$(SMOLRTSP)/build/_deps/metalang99-src/include \
               -DSMOLRTSP_WITH_H264
# The source file of a POC3_ORDER entry (shared module of src/, else the platform directory, else the model directory), for the hash inputs.
poc3_src = $(if $(filter c:%,$(1)),$(call common_src,$(patsubst c:%,%,$(1))),$(firstword $(wildcard $(1).c $(MODEL_DIR)/$(1).c)))
POC3_SRCS := $(foreach o,$(POC3_ORDER),$(call poc3_src,$(o)))
POC3_INCDIRS := . $(MODEL_DIR) $(MODEL_INC_DIRS) $(foreach d,$(SRC_SUBDIRS),$(SRC_DIR)/$(d))
POC3_OBJS := $(foreach o,$(POC3_ORDER),$(call poc3_obj,$(o))) \
             $(addprefix $(POC3_OBJ)/smol_,$(subst /,_,$(patsubst $(SMOLRTSP)/src/%.c,%.o,$(SMOL_SRC))))
# (the smol_ object names flatten the sub directories, see the rules below)

# SmolRTSP's parser uses the ctype macros, which this toolchain's headers turn into __ctype_b_loc(), a symbol the
# camera's libc does not export (see ctype_compat.h). Only the SmolRTSP objects need it (rtsp_auth.c defines
# _GNU_SOURCE itself and must not see a ctype.h included before that); `make poc3-symcheck` catches any other user.
$(POC3_OBJ)/smol_%.o: POC3_CFLAGS += -include ctype_compat.h

define poc3_check
	@test -x "$(CC)" || { echo "toolchain missing: run fetch_toolchain.sh"; exit 1; }
	@test -d "$(SMOLRTSP)/build/_deps" || { echo "smolrtsp checkout missing: pass SMOLRTSP=/path/to/third_party/smolrtsp"; exit 1; }
	@mkdir -p $(POC3_OBJ)
endef

$(POC3_OBJ)/own_%.o: %.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
$(POC3_OBJ)/own_%.o: $(MODEL_DIR)/%.c
	$(poc3_check)
	$(CC) $(POC3_CFLAGS) -c -o $@ $<
define poc3_shared_rule
$$(POC3_OBJ)/ak_%.o: $$(SRC_DIR)/$(1)/%.c
	$$(poc3_check)
	$$(CC) $$(POC3_CFLAGS) -c -o $$@ $$<
endef
$(foreach d,$(SRC_SUBDIRS),$(eval $(call poc3_shared_rule,$(d))))

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

$(OUT)/akamd: $(POC3_OBJS)
	@test -d "$(VENDOR)" || { echo "vendor libs missing: run prepare_vendor.sh"; exit 1; }
	$(CC) $(LDFLAGS) -Wl,--gc-sections -pthread -o $@ $^ $(LDLIBS) $(NPULIBS)

# Every libc symbol the binary needs must be exported by the CAMERA's libc, not only by the build toolchain's.
#   make poc3-symcheck CAMERA_LIBC=/path/to/libuClibc-1.0.31.so   (from your own camera backup)
# APP is the main program, HELPER_BINS the helper programs of the model (HELPERS in model.mk).
APP := akamd
HELPER_BINS := $(foreach h,$(HELPERS),$(word 2,$(subst :, ,$(h))))
ALL_BINS := $(APP) $(HELPER_BINS)
CAMERA_LIBC ?=
poc3-symcheck: $(addprefix $(OUT)/,$(ALL_BINS))
	@test -f "$(CAMERA_LIBC)" || { echo "pass CAMERA_LIBC=<libuClibc-*.so taken from your own camera backup>"; exit 1; }
	@for b in $(ALL_BINS); do $(TC)/bin/arm-linux-readelf --dyn-syms $(OUT)/$$b | awk '$$7=="UND"{print $$8}' | sed 's/@.*//' | grep -v -E '^(ak_|_ITM|__deregister_frame_info|__register_frame_info|$$)'; done | sort -u >$(OUT)/.und
	@$(TC)/bin/arm-linux-readelf --dyn-syms $(CAMERA_LIBC) | awk '$$7!="UND"{print $$8}' | sed 's/@.*//' | sort -u >$(OUT)/.exp
	@missing=$$(comm -23 $(OUT)/.und $(OUT)/.exp); if [ -n "$$missing" ]; then echo "NOT exported by the camera libc: $$missing"; exit 1; else echo "poc3-symcheck: all libc symbols are exported by the camera libc"; fi

# Card input for build_card.sh (--own-app-bin / dist/rot2k-app): runs the symcheck (so CAMERA_LIBC is required), strips the binaries and writes
# BUILDINFO.txt next to them (sha256 of the stripped binary, hash of the sources, the proof that the symcheck passed and against which libc),
# all in $(OUT) = build/<model>; then PUBLISHES a copy of the binaries and BUILDINFO.txt to $(PUBLISH_DIR) (the location build_card.sh,
# build_appfs.sh and make_kit_2k.sh read, so they never look into build/). build_card.sh refuses a binary without that proof or with a stale hash.
# The BUILDINFO keys of a helper are <key>_source_sha256, <key>_sha256, <key>_size (HELPERS entries are key:binary:source_hash.sh argument).
poc3-dist: poc3-symcheck
	@$(TC)/bin/arm-linux-strip $(addprefix $(OUT)/,$(ALL_BINS))
	@set -e; for b in $(ALL_BINS); do \
	    interp=$$($(TC)/bin/arm-linux-readelf -l $(OUT)/$$b | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p'); \
	    test "$$interp" = /lib/ld-uClibc.so.0 || { echo "unexpected interpreter of $$b: $$interp"; exit 1; }; \
	done
	@{ echo "tool=$(APP)"; \
	  echo "source_sha256=$$(sh $(MODEL_DIR)/image/source_hash.sh)"; \
	  echo "sha256=$$(sha256sum $(OUT)/$(APP) | cut -d' ' -f1)"; \
	  echo "size=$$(stat -c %s $(OUT)/$(APP))"; \
	  echo "interpreter=/lib/ld-uClibc.so.0"; \
	  echo "symcheck=passed"; \
	  for h in $(HELPERS); do \
	    k=$${h%%:*}; r=$${h#*:}; b=$${r%%:*}; a=$${r#*:}; \
	    echo "$${k}_source_sha256=$$(sh $(MODEL_DIR)/image/source_hash.sh $$a)"; \
	    echo "$${k}_sha256=$$(sha256sum $(OUT)/$$b | cut -d' ' -f1)"; \
	    echo "$${k}_size=$$(stat -c %s $(OUT)/$$b)"; \
	  done; \
	  echo "camera_libc_sha256=$$(sha256sum $(CAMERA_LIBC) | cut -d' ' -f1)"; \
	} >$(OUT)/BUILDINFO.txt
	@mkdir -p $(PUBLISH_DIR)
	@cp $(addprefix $(OUT)/,$(ALL_BINS)) $(OUT)/BUILDINFO.txt $(PUBLISH_DIR)/
	@echo "poc3-dist: $(OUT)/{$(ALL_BINS)} + BUILDINFO.txt, published to $(PUBLISH_DIR)"

# Hash inputs (image/source_hash.sh, tests/rot2k_source_hash_test.sh): what akamd and akam-led are compiled from, one path per line, and
# the quoted-include search path. They need no compiler and no checkout of SmolRTSP.
.PHONY: print-sources print-incdirs print-led-sources
print-sources:
	@printf '%s\n' $(foreach f,$(POC3_SRCS),$(abspath $(f)))
print-incdirs:
	@printf '%s\n' $(foreach d,$(POC3_INCDIRS),$(abspath $(d)))
print-led-sources:
	@printf '%s\n' $(foreach f,$(LED_SRC),$(abspath $(f)))

-include $(POC3_OBJS:.o=.d)

clean:
	rm -rf $(OUT)
