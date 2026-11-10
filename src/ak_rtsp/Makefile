ZIG    ?= zig
CC      = $(ZIG) cc -target arm-linux-musleabi -mcpu=arm926ej_s
CFLAGS  = -std=gnu99 -mfloat-abi=soft -O2 -Wall -g \
          -I. \
          -I../smolrtsp/include \
          -I../smolrtsp/build/_deps/slice99-src \
          -I../smolrtsp/build/_deps/datatype99-src \
          -I../smolrtsp/build/_deps/interface99-src \
          -I../smolrtsp/build/_deps/metalang99-src/include \
          -DSMOLRTSP_WITH_H264
LDFLAGS = -pthread -static

TARGET     = ak_rtsp
TARGET_DBG = isp_dump
TARGET_HOOK = isp_hook.so
TARGET_STOCK = stock_hook.so
TARGET_TOOL = flash_tool
TARGET_DIV0 = patch_div0
.DEFAULT_GOAL := all

# -----------------------------------------------------------------------
# Build versioning
#
# build_number.txt  — persists the counter across builds (not committed)
# version_info.h    — regenerated on every make by gen_version.sh
#
# version.o has a FORCE dependency so it always recompiles. Its recipe
# calls gen_version.sh (POSIX sh) which:
#   1. increments build_number.txt
#   2. writes version_info.h with BUILD_NUMBER / BUILD_TIMESTAMP / GIT_HASH
# -----------------------------------------------------------------------
BUILD_NUM_FILE := build_number.txt

FORCE:
.PHONY: FORCE all clean upload firmware

# time32_shim.o replaces musl's clock_gettime()/timerfd_*(), which use time64
# syscalls the camera's kernel acknowledges without executing. EVERY static
# binary below must link it (see ../common/time32_shim.c).
SHIM    = ../common/time32_shim.o

ASM_SRCS = arm_atomics.S
SRCS    = main.c \
          isp.c \
          isp_blocks.c \
          imgq.c \
          vi.c \
          venc.c \
          audio.c \
          audio_out.c \
          rtsp.c \
          h264_params.c \
          rtp_clock.c \
          net_tx.c \
          rtsp_in.c \
          rtsp_idle.c \
          rtsp_sdp.c \
          rtp_in.c \
          audio_vad.c \
          talk.c \
          md5.c \
          rtsp_auth.c \
          sha1.c \
          b64.c \
          cred.c \
          ircut.c \
          leds.c \
          ledsys.c \
          ae.c \
          aec_custom.c \
          night.c \
          night_logic.c \
          g711.c \
          audio_jitter.c \
          control.c \
          isp_tune.c \
          ctl_gate.c \
          ctl_sys.c \
          version.c \
          $(wildcard ../smolrtsp/src/*.c) \
          $(wildcard ../smolrtsp/src/types/*.c) \
          $(wildcard ../smolrtsp/src/writer/*.c) \
          $(wildcard ../smolrtsp/src/transport/*.c) \
          ../smolrtsp/src/nal/h264.c
OBJS    = $(SRCS:.c=.o) $(ASM_SRCS:.S=.o) $(SHIM)

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

$(TARGET_HOOK): isp_hook.c
	$(CC) -std=gnu99 -mfloat-abi=soft -O1 -fPIC -shared -nostdlib -ffreestanding \
	      -mcpu=arm926ej_s -o $@ $<

# stock_hook.so: capture of the stock anyka_ipc (speaker, GPIO, ISP), see docs/stock_capture.md.
# Includes isp_hook.c, needs no libc and no compiler-rt (no division by a variable,
# no memcpy), exports only the hooked libc symbols, so the only undefined symbol left
# is the weak __errno_location (checked by tests/stock_hook_test.sh).
STOCK_FLAGS = -std=gnu99 -mfloat-abi=soft -O1 -Wall -Wextra -fPIC -shared -nostdlib \
              -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden \
              -fno-unwind-tables -fno-asynchronous-unwind-tables -rtlib=none -g0 \
              -mcpu=arm926ej_s
$(TARGET_STOCK): stock_hook.c isp_hook.c
	$(CC) $(STOCK_FLAGS) -o $@ stock_hook.c

$(TARGET_DBG): isp_dump.c isp_blocks.c $(SHIM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_TOOL): flash_tool.c arm_atomics.S $(SHIM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET_DIV0): patch_div0.c $(SHIM)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# version.o — always rebuilt (FORCE). Calls gen_version.sh first to
# increment the counter and regenerate version_info.h, then compiles.
version.o: version.c FORCE
	./gen_version.sh
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o ../common/*.o ../smolrtsp/src/*.o ../smolrtsp/src/*/*.o
	rm -f $(TARGET) $(TARGET_DBG) $(TARGET_HOOK) $(TARGET_STOCK) $(TARGET_TOOL) $(TARGET_DIV0) version_info.h

# -----------------------------------------------------------------------
# firmware — builds the flashable, timestamped APP squashfs image
# (ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs, written next to build_firmware.sh
# unless FW_DIR is overridden). Rebuilds ak_rtsp first, then runs
# build_firmware.sh (needs mksquashfs/unsquashfs on the host). Requires an
# app_extracted/ dump of your own camera's APP partition — see
# docs/building_your_own_firmware.md for the full walkthrough and what
# PATCHED/FW_DIR should point at.
# -----------------------------------------------------------------------
firmware: $(TARGET)
	bash build_firmware.sh

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
	         $(FLASH_SCRIPTS)/install_with_flash_tool.sh \
	         $(FLASH_SCRIPTS)/quiet_and_flash_test.sh; do \
	    b=$$(basename "$$f"); \
	    echo "upload $$b"; \
	    ssh "root@$(CAMERA_IP)" "cat > /tmp/$$b && chmod +x /tmp/$$b" < "$$f"; \
	done
