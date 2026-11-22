# Shared sources of src/, listed once for every firmware Makefile (docs/archive/convergence-plan.md 2.4).
# The includer sets SRC_DIR to the path of src/ as seen from its own working directory, then names the files it builds by
# basename: $(call common_src,rtp_clock) is the path of the .c, $(call common_obj,rtp_clock) the .o next to it (for
# per-object flags such as `$(OBJS_X): CFLAGS += -Os`), and $(common_inc) the include path every user of a shared header
# needs (headers keep being included by basename). The table below is the ONLY place that knows which directory of src/
# a module lives in: a module moves by changing its line here.
# A file is added here only if it has no vendor header, no device path and no ioctl, and a host test runs it.

SRCDIR_api := json_min api_sig api_keys api_limit api_events api_core api_httpd
SRCDIR_core := settings_reg cfg_file klog time32_shim
SRCDIR_core_control := ctl_gate ctl_sys
SRCDIR_core_version := version
SRCDIR_features_audio := audio_jitter audio_vad g711
SRCDIR_features_button := button_logic button_gesture button_cfg
SRCDIR_features_light := light_logic led_pat
SRCDIR_features_night := night_logic
SRCDIR_features_ptz := ptz_vel ptz_lease ptz_patrol
SRCDIR_features_track := drift_match track_logic track_relay
SRCDIR_features_scene := scene_check
SRCDIR_hal := hal_sys hal_input
SRCDIR_net_crypto := md5 sha1 b64 cred sha256
SRCDIR_net_http := http_req
SRCDIR_net_mqtt := mqtt mqtt_ha
SRCDIR_net_onvif := http_srv soap onvif wsd onvif_common
SRCDIR_net_rtsp := h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtsp_busy rtp_clock tx_pace net_tx rtp_in h264_nalu rtcp_sr

SRC_SUBDIRS := api core core/control core/version features/audio features/button features/light features/night features/ptz features/scene features/track hal net/crypto net/http net/mqtt net/onvif net/rtsp

# common_src <name>: the path of src/<dir>/<name>.c, found through the SRCDIR_* lists above.
common_dir = $(or $(strip $(foreach d,$(SRC_SUBDIRS),$(if $(filter $(1),$(SRCDIR_$(subst /,_,$(d)))),$(d)))),$(error not a shared source: $(1)))
common_src = $(SRC_DIR)/$(call common_dir,$(1))/$(1).c
common_obj = $(patsubst %.c,%.o,$(call common_src,$(1)))
common_inc = $(foreach d,$(SRC_SUBDIRS),-I$(SRC_DIR)/$(d))
