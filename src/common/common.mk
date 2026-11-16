# Shared sources of src/common, listed once for every firmware Makefile (docs/convergence-plan.md 2.4).
# The includer sets COMMON_DIR to the path of src/common as seen from its own working directory, then names the
# files it builds by basename: $(call common_src,rtp_clock) is the path of the .c, $(call common_obj,rtp_clock) the
# .o next to it (for per-object flags such as `$(COMMON_OBJS_X): CFLAGS += -Os`), and $(common_inc) the include path
# every user of a common header needs (headers keep being included by basename).
# A file is added here only if it has no vendor header, no device path and no ioctl, and a host test runs it.

COMMON_NET   := h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtsp_busy rtp_clock tx_pace net_tx md5 sha1 b64 cred \
                mqtt mqtt_ha
COMMON_LOGIC := night_logic cfg_file klog audio_jitter audio_vad g711

common_src = $(or $(and $(filter $(1),$(COMMON_NET)),$(COMMON_DIR)/net/$(1).c),$(and $(filter $(1),$(COMMON_LOGIC)),$(COMMON_DIR)/logic/$(1).c),$(error not a src/common source: $(1)))
common_obj = $(patsubst %.c,%.o,$(call common_src,$(1)))
common_inc = -I$(COMMON_DIR)/net -I$(COMMON_DIR)/logic
