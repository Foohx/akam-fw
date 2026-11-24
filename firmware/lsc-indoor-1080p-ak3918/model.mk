# Model description of the LSC indoor 1080p (AK3918), included by platforms/anyka-ak3918/Makefile (see its header).
# Holds what is specific to this camera: the features, which objects get which flags, and the ORDERED source list. The order is the
# link order and the link order sets the layout of the stripped binary (--gc-sections): never sort it, never use wildcards for it.
# Paths: bare names are platform sources (platforms/anyka-ak3918), $(MODEL_DIR)/... this directory, $(call common_src,x) a shared
# module of src/ (found by src/build/sources.mk).

# Features of this model (src/build/features.mk): only what the 1080p has hardware or a use for.
# Everything else (PTZ, button, scene, patrol, tracking) is OFF: nothing of it is compiled into the binary.
AKAM_FEAT_SYS    := 1
AKAM_FEAT_LIGHT  := 1
AKAM_FEAT_API    := 1
AKAM_FEAT_PTZ    := 0
AKAM_FEAT_BUTTON := 0
AKAM_FEAT_SCENE  := 0
AKAM_FEAT_PATROL := 0
AKAM_FEAT_TRACK  := 0
AKAM_FEAT_ONVIF  := 1

# AKAM_FEAT_ONVIF := 0 drops the ONVIF objects and links onvif_off.c (the answers of an ONVIF that is off).
# ONVIF is off the video path and opt-in: size over speed for its files only.
ONVIF_OBJS = $(call common_obj,http_srv) $(call common_obj,soap) $(call common_obj,onvif) onvif_cfg.o $(MODEL_DIR)/onvif_model.o onvif_device.o onvif_media.o onvif_imaging.o $(call common_obj,wsd) $(call common_obj,cred)
# MQTT is off the video path and opt-in as well: size over speed, and stack canaries (it parses broker input).
MQTT_OBJS = $(call common_obj,mqtt) $(call common_obj,mqtt_ha) mqtt_svc.o mqtt_ops.o
# The signed control API (port 8092, off until an API key exists) is off the video path as well: size over speed, and stack canaries
# below (it parses the network). The same shared sources the 2K links (src/api, net/http/http_req.c).
API_OBJS = $(call common_obj,json_min) $(call common_obj,settings_reg) $(call common_obj,api_keys) $(call common_obj,sha256) \
           $(call common_obj,api_sig) $(call common_obj,api_limit) $(call common_obj,api_events) $(call common_obj,http_req) \
           $(call common_obj,api_core) $(call common_obj,api_httpd) $(MODEL_DIR)/settings.o $(MODEL_DIR)/api_bind.o
# G17: stack canaries only on the parsers that read untrusted network input (ONVIF/SOAP/HTTP, RTSP auth,
# the 8091 line parser). Everywhere would cost about 3 % of the stripped binary and a canary check on the
# video/audio hot paths; here it costs under 1 %. Static non-PIE musl: no RELRO or PIE to turn on, and
# musl ignores _FORTIFY_SOURCE, so those are deliberately not set.
HARDENED_OBJS = $(ONVIF_OBJS) $(MQTT_OBJS) $(call common_obj,http_req) $(call common_obj,api_core) $(call common_obj,api_httpd) $(call common_obj,api_sig) $(call common_obj,rtsp_auth) control.o $(call common_obj,rtsp_in) $(call common_obj,rtsp_busy) $(call common_obj,rtp_in) $(call common_obj,ctl_gate)

# Source list in link order. A shared module is added through $(call common_src,<name>); a feature that is off contributes nothing.
SRCS     = main.c \
          isp.c \
          isp_blocks.c \
          isp_nr.c \
          isp_nr_ctl.c \
          isp_wdr.c \
          imgq.c \
          vi.c \
          venc.c \
          audio.c \
          $(call common_src,audio_out) \
          rtsp_session.c \
          $(call common_src,h264_params) \
          $(call common_src,h264_nalu) \
          $(call common_src,rtcp_sr) \
          $(call common_src,rtp_clock) \
          $(call common_src,net_tx) \
          $(call common_src,tx_pace) \
          $(call common_src,rtsp_in) \
          $(call common_src,rtsp_idle) \
          $(call common_src,rtsp_busy) \
          $(call common_src,rtsp_sdp) \
          $(call common_src,rtp_in) \
          $(call common_src,audio_vad) \
          $(call common_src,talk) \
          $(call common_src,md5) \
          $(call common_src,rtsp_auth) \
          $(call common_src,sha1) \
          $(call common_src,b64) \
          $(call common_src,cred) \
          $(call feat_objs,ONVIF,$(call common_src,http_srv) $(call common_src,soap) $(call common_src,onvif) $(call common_src,onvif_common)) \
          $(call feat_objs,ONVIF,onvif_cfg.c $(MODEL_DIR)/onvif_model.c onvif_device.c onvif_media.c onvif_imaging.c) \
          $(call feat_objs,ONVIF,$(call common_src,wsd)) \
          $(if $(call feat_on,ONVIF),,onvif_off.c) \
          $(call common_src,mqtt) \
          $(call common_src,mqtt_ha) \
          mqtt_svc.c \
          mqtt_ops.c \
          ircut.c \
          leds.c \
          $(call feat_objs,LIGHT,$(call common_src,led_pat) $(call common_src,light_logic)) \
          ledsys.c \
          ae.c \
          aec_custom.c \
          night.c \
          $(call common_src,night_logic) \
          $(call common_src,cfg_file) \
          $(call common_src,g711) \
          $(call common_src,audio_jitter) \
          control.c \
          swdog.c \
          $(call common_src,klog) \
          klog_run.c \
          isp_tune.c \
          image_orient.c \
          video_cfg.c \
          video_ctl.c \
          $(call common_src,ctl_gate) \
          $(call feat_objs,API,$(call common_src,json_min) $(call common_src,settings_reg) $(call common_src,api_keys) $(call common_src,sha256) \
              $(call common_src,api_sig) $(call common_src,api_limit) $(call common_src,api_events) $(call common_src,http_req) \
              $(call common_src,api_core) $(call common_src,api_httpd)) \
          $(MODEL_DIR)/settings.c \
          $(MODEL_DIR)/api_bind.c \
          $(call common_src,ctl_sys) \
          $(call feat_objs,SYS,$(call common_src,hal_sys)) \
          $(call common_src,version) \
          $(wildcard $(ROOT)/third_party/smolrtsp/src/*.c) \
          $(wildcard $(ROOT)/third_party/smolrtsp/src/types/*.c) \
          $(wildcard $(ROOT)/third_party/smolrtsp/src/writer/*.c) \
          $(wildcard $(ROOT)/third_party/smolrtsp/src/transport/*.c) \
          $(ROOT)/third_party/smolrtsp/src/nal/h264.c
