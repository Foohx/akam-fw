/* The ONVIF device / media handlers whose reply is the same on both platforms (read-only device, fixed profiles, no audio configuration). The
 * platform services (onvif_device.c / onvif_media.c on the 1080p, onvif_services.c on the 2K) put them in their operation tables and keep every
 * handler whose text depends on the camera (resolution, bit rate, stream URI, model strings). */
#ifndef AKAM_ONVIF_COMMON_H
#define AKAM_ONVIF_COMMON_H

#include "onvif.h"

/* Faults: ter:NoProfile / ter:NoConfig (token not found), and the refusals of a read-only device. */
void onvif_fault_noprofile(onvif_call_t *c);
void onvif_fault_noconfig(onvif_call_t *c);
void onvif_op_refuse_set(onvif_call_t *c);    /* Set<Video|Audio>...Configuration: ter:ConfigModify */
void onvif_op_refuse_create(onvif_call_t *c); /* CreateProfile: ter:MaxNVTProfiles */
void onvif_op_refuse_delete(onvif_call_t *c); /* DeleteProfile: ter:DeletionOfFixedProfile */

/* Media: the audio source / encoder configuration lists are empty (the microphone is L16, which ONVIF Media 1 cannot name). */
void onvif_op_get_asrc_configs(onvif_call_t *c);
void onvif_op_get_aenc_configs(onvif_call_t *c);

/* Device: GetDiscoveryMode answers Discoverable. */
void onvif_op_get_discovery_mode(onvif_call_t *c);

#endif
