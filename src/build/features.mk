# Build-time feature selection (docs/archive/convergence-plan.md 2.5, docs/hal-architecture.md "Feature selection").
# A model Makefile sets AKAM_FEAT_<NAME> := 1 (on) or 0 (off) BEFORE including this file, then adds a module's objects only
# through $(call feat_objs,<NAME>,<objects>): a feature that is off contributes no object, so no dead code ships in a model
# that does not use it. A feature the model does not set is off. Features:
#   API      control API v1 (HMAC auth, rate limit, settings registry, HTTP server)
#   PTZ      pan/tilt/zoom motor driver and its logic
#   BUTTON   physical button input
#   SCENE    scene detection (day/night/lighting) logic
#   PATROL   PTZ patrol / preset tour
#   TRACK    motion tracking
#   SYS      system metrics sampler (hal_sys: CPU, load, memory, uptime, Wi-Fi level)
#   LIGHT    light emitters (hal_light: IR LED, status LED) and their logic
#   ONVIF    ONVIF core (http_srv, soap, onvif, wsd, src/net) and, per model, its services (the model binding, onvif.h)
# Adding a feature = add its name here, set it per model, gate its objects with feat_objs. Nothing here changes a binary:
# listing objects directly and listing them through feat_objs with the feature on link identically.

AKAM_FEATURES := API PTZ BUTTON SCENE PATROL TRACK SYS LIGHT ONVIF

# feat_on <NAME>: non-empty when the feature is enabled.
feat_on   = $(filter 1,$(AKAM_FEAT_$(1)))
# feat_objs <NAME>,<objects>: the objects when the feature is on, nothing otherwise.
feat_objs = $(if $(call feat_on,$(1)),$(2))

# Every AKAM_FEAT_* must be 0 or 1 and name a known feature (typo guard: an unknown name would silently build nothing).
$(foreach v,$(filter AKAM_FEAT_%,$(.VARIABLES)),\
  $(if $(filter $(patsubst AKAM_FEAT_%,%,$(v)),$(AKAM_FEATURES)),,$(error unknown feature $(v), known: $(AKAM_FEATURES))) \
  $(if $(filter 0 1,$($(v))),,$(error $(v) must be 0 or 1, not '$($(v))')))

# `make features`: the features this build has on and off.
.PHONY: features
features:
	@echo "on : $(foreach f,$(AKAM_FEATURES),$(if $(call feat_on,$(f)),$(f)))"
	@echo "off: $(foreach f,$(AKAM_FEATURES),$(if $(call feat_on,$(f)),,$(f)))"
