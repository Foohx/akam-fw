/* ONVIF Media service (version 1, namespace trt) at /onvif/media_service (specs/onvif.md).
 *
 * One fixed profile "main": video source "vsrc" (1920x1080, configured fps), video encoder configuration "venccfg" (H.264
 * Main, 1920x1080, frame rate / bitrate cap / GOP length from video_cfg.h: the values venc.c sends to the encoder; the
 * defaults are 15 fps, ENC_KBPS and GOP ENC_GOP), stream
 * rtsp://<address the client used>:554/ . The device is read only: every Set/Add/Remove/CreateProfile/DeleteProfile
 * is refused with a fault after the access gate (WRITE class: refused when the RTSP password is off).
 *
 * Audio, described honestly:
 *  - The microphone is L16 8 kHz mono on RTSP (streamid=1). ONVIF Media 1 can describe an audio encoder only as
 *    G711, G726 or AAC, so L16 cannot be described: the profile has NO audio source and NO audio encoder
 *    configuration (GetAudioSourceConfigurations and GetAudioEncoderConfigurations are empty). GetAudioSources
 *    still lists the physical input ("asrc", 1 channel): that is a fact, not an encoding. The audio is in the
 *    RTSP SDP anyway, which is what clients play.
 *  - The speaker and the G.711 backchannel (streamid=2, spec section 1) are described, but only while the
 *    backchannel can really be served, i.e. the RTSP password is ON and audio_out_talk_available() (audio.out_enable
 *    and audio.talk_enable): audio output "aout", output configuration "aoutcfg", decoder configuration "adec",
 *    both in the profile. Otherwise those lists are empty and the profile has no audio output (the same rule as
 *    the 551 answer of rtsp.c).
 * Snapshots do not exist (no JPEG path, spec U14): GetSnapshotUri is a fault and the capability says false. */
#pragma once

/* Called by onvif_start. 0 or -1. */
int onvif_media_register(void);
