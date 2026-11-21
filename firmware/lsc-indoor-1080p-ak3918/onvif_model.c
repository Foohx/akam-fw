/* ONVIF model binding of the 1080p (see onvif.h in src/net): the services this camera offers. */
#include "onvif.h"
#include "onvif_device.h"
#include "onvif_imaging.h"
#include "onvif_media.h"

const onvif_register_fn onvif_model_services[] = { onvif_device_register, onvif_media_register, onvif_imaging_register, NULL };
