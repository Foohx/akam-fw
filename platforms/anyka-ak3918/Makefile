# Build of the Anyka AK3918 "direct drivers" platform (raw kernel ioctls, static musl, pinned Zig): the program of the LSC indoor 1080p,
# shipped under the name ak_rtsp (renamed in a later step). Run from this directory: `make -C platforms/anyka-ak3918 [all|repro|firmware|...]`
# (the root Makefile delegates: `make 1080p`, `make repro-1080p`). What differs per camera model lives in firmware/<MODEL>/model.mk
# (features, the ordered object list); the shared modules are named by src/build/sources.mk; the generic rules and flags are here.
ZIG    ?= zig
ROOT    = ../..
SRC_DIR = $(ROOT)/src
MODEL  ?= lsc-indoor-1080p-ak3918
MODEL_DIR = $(ROOT)/firmware/$(MODEL)
include $(SRC_DIR)/build/sources.mk
# Everything a build writes goes under build/$(MODEL) (out of tree, git-ignored; two models never overwrite each other): the binaries
# (ak_rtsp, ak_rtsp.repro, flash_tool, isp_dump, patch_div0, the two hook libraries), version_info.h, and the objects, mirroring the source tree below the repository root: a source named
# ../../src/net/rtsp/x.c gets build/$(MODEL)/src/net/rtsp/x.o, a platform source x.c gets build/$(MODEL)/x.o.
# `$(call O,a.o ../../b/c.o)` maps source-relative object names to those paths.
BUILD ?= $(ROOT)/build/$(MODEL)
O = $(addprefix $(BUILD)/,$(subst ../,,$(1)))
# The model: AKAM_FEAT_* (src/build/features.mk, docs/convergence-plan.md 2.4), the object groups and SRCS, the ordered source list.
include $(MODEL_DIR)/model.mk
include $(SRC_DIR)/build/features.mk
# A model C file must not share its basename with a C file of the platform directory: the platform file would be compiled and the model's ignored
# (or the reverse) without any error. Checked at parse time; tools/ci/layering_lint.sh and tests/layering_lint_test.sh cover it.
$(foreach f,$(notdir $(wildcard $(MODEL_DIR)/*.c)),$(if $(wildcard $(f)),$(error basename clash: $(f) exists in both the platform directory and $(MODEL_DIR))))

CC      = $(ZIG) cc -target arm-linux-musleabi -mcpu=arm926ej_s
CFLAGS  = -std=gnu99 -mfloat-abi=soft -O2 -Wall -Wformat -Wformat-security -g \
          -ffunction-sections -fdata-sections \
          -I. -I$(MODEL_DIR) -I$(BUILD) $(common_inc) \
          -I$(ROOT)/third_party/smolrtsp/include \
          -I$(ROOT)/third_party/smolrtsp/build/_deps/slice99-src \
          -I$(ROOT)/third_party/smolrtsp/build/_deps/datatype99-src \
          -I$(ROOT)/third_party/smolrtsp/build/_deps/interface99-src \
          -I$(ROOT)/third_party/smolrtsp/build/_deps/metalang99-src/include \
          -DSMOLRTSP_WITH_H264
LDFLAGS = -pthread -static -Wl,--gc-sections
# make EXTRA_CFLAGS=-DMQTT_NO_DNS: the MQTT broker must then be an IPv4 address (saves about 14 KB, see mqtt_svc.c)
CFLAGS += $(EXTRA_CFLAGS)

# ONVIF, MQTT and the control API are off the video path and opt-in: size over speed for their files only (the lists are in model.mk).
$(call O,$(ONVIF_OBJS)): CFLAGS += -Os
$(call O,$(MQTT_OBJS)): CFLAGS += -Os
$(call O,$(API_OBJS)): CFLAGS += -Os
# G17 (the comment in model.mk): stack canaries only on the parsers of untrusted network input.
$(call O,$(HARDENED_OBJS)): CFLAGS += -fstack-protector-strong

TARGET     = $(BUILD)/ak_rtsp
TARGET_DBG = $(BUILD)/isp_dump
TARGET_HOOK = $(BUILD)/isp_hook.so
TARGET_STOCK = $(BUILD)/stock_hook.so
TARGET_TOOL = $(BUILD)/flash_tool
TARGET_DIV0 = $(BUILD)/patch_div0
.DEFAULT_GOAL := all

# -----------------------------------------------------------------------
# Build versioning
#
# build/<model>.build_number.txt — persists the counter across builds and cleans (not committed)
# build/<model>/version_info.h   — regenerated on every make by gen_version.sh
#
# version.o has a FORCE dependency so it always recompiles. Its recipe
# calls gen_version.sh (POSIX sh) which:
#   1. increments build_number.txt
#   2. writes version_info.h with BUILD_NUMBER / BUILD_TIMESTAMP / GIT_HASH
# -----------------------------------------------------------------------
BUILD_NUM_FILE := $(ROOT)/build/$(MODEL).build_number.txt

FORCE:
.PHONY: FORCE all clean upload firmware print-incdirs
# The quoted-include search path of this build, one absolute directory per line (tools/ci/layering_lint.sh).
print-incdirs:
	@printf '%s\n' $(foreach d,. $(MODEL_DIR) $(BUILD) $(SRC_SUBDIRS:%=$(SRC_DIR)/%),$(abspath $(d)))

# time32_shim.o replaces musl's clock_gettime()/timerfd_*(), which use time64
# syscalls the camera's kernel acknowledges without executing. EVERY static
# binary below must link it (see src/core/time32_shim.c).
SHIM    = $(BUILD)/src/core/time32_shim.o

ASM_SRCS = arm_atomics.S
OBJS    = $(call O,$(SRCS:.c=.o) $(ASM_SRCS:.S=.o)) $(SHIM)

# Short names of the binaries (`make ak_rtsp flash_tool`): the files themselves are build/$(MODEL)/<name>.
.PHONY: ak_rtsp isp_dump isp_hook.so stock_hook.so flash_tool patch_div0
ak_rtsp: $(TARGET)
isp_dump: $(TARGET_DBG)
isp_hook.so: $(TARGET_HOOK)
stock_hook.so: $(TARGET_STOCK)
flash_tool: $(TARGET_TOOL)
patch_div0: $(TARGET_DIV0)

all: $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_STOCK) $(TARGET_TOOL) $(TARGET_DIV0)

# -----------------------------------------------------------------------
# debug-dump — builds ak_rtsp with DEBUG_IOCTL_DUMP defined, so venc.c logs
# our own CREATE_ENC payload to /tmp/ak_rtsp_ioctl_dump.log in the same
# format isp_hook.c uses for stock's capture (see venc_dump.h). Not part of
# `all`/`$(TARGET)` — the normal build never defines DEBUG_IOCTL_DUMP.
# Forces a full object rebuild first (the macro changes venc.o's contents
# but make has no way to know that from timestamps alone).
# -----------------------------------------------------------------------
.PHONY: debug-dump
debug-dump:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) CFLAGS="$(CFLAGS) -DDEBUG_IOCTL_DUMP"

# -----------------------------------------------------------------------
# aec-custom — builds ak_rtsp with AEC_CUSTOM defined, swapping ae.c's
# on-chip-tick-only AE loop for aec_custom.c's software AEC loop.
# Compile-time only, not a runtime toggle. Not part of `all`/the normal build.
# -----------------------------------------------------------------------
.PHONY: aec-custom
aec-custom:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0) CFLAGS="$(CFLAGS) -DAEC_CUSTOM"

# -----------------------------------------------------------------------
# aec-custom-nosuspend — DIAGNOSTIC ONLY, not for shipping. Same as
# aec-custom but skips the ISP_INNER_AE_SUSPEND call, to isolate whether
# AE_SUSPEND itself (vs. our own control loop) is responsible for the much
# heavier div0 flood observed with aec-custom.
# -----------------------------------------------------------------------
.PHONY: aec-custom-nosuspend
aec-custom-nosuspend:
	"$(MAKE)" clean
	"$(MAKE)" $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0) CFLAGS="$(CFLAGS) -DAEC_CUSTOM -DAEC_SKIP_SUSPEND"

$(TARGET_HOOK): tools/isp_hook.c
	@mkdir -p $(@D)
	$(CC) -std=gnu99 -mfloat-abi=soft -O1 -fPIC -shared -nostdlib -ffreestanding \
	      -mcpu=arm926ej_s -o $@ $<

# stock_hook.so: capture of the stock anyka_ipc (speaker, GPIO, ISP), see docs/manual/stock-capture.md.
# Includes isp_hook.c, needs no libc and no compiler-rt (no division by a variable,
# no memcpy), exports only the hooked libc symbols, so the only undefined symbol left
# is the weak __errno_location (checked by tests/stock_hook_test.sh).
STOCK_FLAGS = -std=gnu99 -mfloat-abi=soft -O1 -Wall -Wextra -fPIC -shared -nostdlib \
              -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden \
              -fno-unwind-tables -fno-asynchronous-unwind-tables -rtlib=none -g0 \
              -mcpu=arm926ej_s
$(TARGET_STOCK): tools/stock_hook.c tools/isp_hook.c
	@mkdir -p $(@D)
	$(CC) $(STOCK_FLAGS) -o $@ tools/stock_hook.c

$(TARGET_DBG): tools/isp_dump.c isp_blocks.c $(SHIM)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_TOOL): $(MODEL_DIR)/image/flash_tool.c arm_atomics.S $(SHIM)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_DIV0): tools/patch_div0.c $(SHIM)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# One rule per source directory: the object path drops the leading ../ of the source (see O above).
$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# One rule per source tree below the repository root (the object path drops the leading ../ of the source, see O above).
$(BUILD)/src/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/firmware/%.o: $(ROOT)/firmware/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/third_party/%.o: $(ROOT)/third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# version.o — always rebuilt (FORCE). Calls gen_version.sh first to
# increment the counter and regenerate version_info.h, then compiles.
$(BUILD)/src/core/version/version.o: $(SRC_DIR)/core/version/version.c FORCE
	@mkdir -p $(@D)
	@mkdir -p $(dir $(BUILD_NUM_FILE))
	AKAM_VERSION_HEADER=$(BUILD)/version_info.h AKAM_BUILD_NUMBER_FILE=$(BUILD_NUM_FILE) sh $(SRC_DIR)/core/version/gen_version.sh
	$(CC) $(CFLAGS) -c -o $@ $<

# -----------------------------------------------------------------------
# repro — reproducible, stripped 1080p ak_rtsp (docs/convergence-plan.md 2.5, tools/ci/repro_check.sh).
# Fixed version stamp (AKAM_REPRO=1, see gen_version.sh), no absolute paths (-ffile-prefix-map), no debug info,
# the frozen SRCS link order, then `zig objcopy --strip-all`. Output: ak_rtsp.repro, its sha256 is printed.
# The normal build is untouched: only the stamp and the paths differ.
# -----------------------------------------------------------------------
REPRO_ROOT = $(abspath ../..)
REPRO_FLAGS = -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(REPRO_ROOT)=/src -g0
REPRO_EPOCH ?= 0
.PHONY: repro
repro:
	"$(MAKE)" clean
	AKAM_REPRO=1 SOURCE_DATE_EPOCH=$(REPRO_EPOCH) "$(MAKE)" $(TARGET) CFLAGS="$(CFLAGS) $(REPRO_FLAGS)"
	$(ZIG) objcopy --strip-all $(TARGET) $(TARGET).repro
	@sha256sum $(TARGET).repro

clean:
	rm -rf $(BUILD)
	@# outputs of builds from before the per-model build directory
	rm -f *.o ak_rtsp ak_rtsp.repro isp_dump isp_hook.so stock_hook.so flash_tool patch_div0 version_info.h

# -----------------------------------------------------------------------
# firmware — builds the flashable, timestamped APP squashfs image
# (ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs, written next to build_image.sh
# unless FW_DIR is overridden). Rebuilds ak_rtsp first, then runs
# build_image.sh (needs mksquashfs/unsquashfs on the host). Requires an
# app_extracted/ dump of your own camera's APP partition — see
# docs/manual/build.md for the full walkthrough and what
# PATCHED/FW_DIR should point at.
# -----------------------------------------------------------------------
firmware: $(TARGET)
	bash $(MODEL_DIR)/image/build_image.sh

# Upload the binaries + launch scripts to the camera over SSH into /tmp.
# CAMERA_IP has no default: `make upload CAMERA_IP=<camera address>`.
# Plain `ssh ... 'cat > file'` is used instead of scp because OpenSSH >= 9
# scp speaks SFTP, which Dropbear does not provide.
FLASH_SCRIPTS = ../../tools/flash_scripts
upload: $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0)
	@test -n "$(CAMERA_IP)" || { echo "usage: make upload CAMERA_IP=<camera address>" >&2; exit 1; }
	@set -e; \
	for f in $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_TOOL) $(TARGET_DIV0) \
	         $(FLASH_SCRIPTS)/start_rtsp.sh \
	         $(FLASH_SCRIPTS)/flash_guard.sh \
	         $(FLASH_SCRIPTS)/install_with_flash_tool.sh; do \
	    b=$$(basename "$$f"); \
	    echo "upload $$b"; \
	    ssh "root@$(CAMERA_IP)" "cat > /tmp/$$b && chmod +x /tmp/$$b" < "$$f"; \
	done
