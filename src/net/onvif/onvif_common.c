#include "onvif_common.h"

#define ENV_OPEN ONVIF_ENV_OPEN
#define ENV_CLOSE ONVIF_ENV_CLOSE

void onvif_fault_noprofile(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoProfile", "The requested profile token does not exist");
}

void onvif_fault_noconfig(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:NoConfig", "The requested configuration token does not exist");
}

void onvif_op_refuse_set(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:ConfigModify", "The configuration cannot be changed (read-only device)");
}

void onvif_op_refuse_create(onvif_call_t *c)
{
    onvif_fault(c, 1, "ter:Action", "ter:MaxNVTProfiles", "The maximum number of profiles is already reached");
}

void onvif_op_refuse_delete(onvif_call_t *c)
{
    onvif_fault(c, 0, "ter:InvalidArgVal", "ter:DeletionOfFixedProfile", "The profile is fixed");
}

void onvif_op_get_asrc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetAudioSourceConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}

void onvif_op_get_aenc_configs(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<trt:GetAudioEncoderConfigurationsResponse/>" ENV_CLOSE);
    onvif_send(c);
}

void onvif_op_get_discovery_mode(onvif_call_t *c)
{
    onvif_reply(c, ENV_OPEN "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>" ENV_CLOSE);
    onvif_send(c);
}
