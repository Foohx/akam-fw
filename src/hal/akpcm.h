/* The capture side of the Anyka "ak_pcm" audio driver (/dev/pcmC0D0c), shared by both platforms: the 1080p (platforms/anyka-ak3918/audio.c) and the
 * 2K (platforms/anyka-ak3918av130/audio.c, firmware/<2k>/helpers/mic). Request numbers and the parameter block as observed on the camera (magic byte
 * 'P' = 0x50); platforms/anyka-ak3918/ak_ioctls.h documents each of them at length. Constants and one struct only: no call. */
#ifndef AKAM_HAL_AKPCM_H
#define AKAM_HAL_AKPCM_H

#include <stdint.h>

/* GET_PARS is MISNAMED: despite the _IOR-shaped number it is the commit of a struct akpcm_pars (the driver copies it FROM user space). */
#define AKPCM_IOC_GET_PARS      0x401c50e0  /* _IOR('P',0xe0,28) */
#define AKPCM_IOC_SET_RATE      0x800450ea  /* _IOW('P',0xea,4) */
#define AKPCM_IOC_SET_STATE     0x80045010  /* _IOW('P',0x10,4) */
#define AKPCM_IOC_GET_STATE     0x40045010  /* _IOR('P',0x10,4) */
#define AKPCM_IOC_SET_PERIODS   0x80045030  /* _IOW('P',0x30,4) */
#define AKPCM_IOC_GET_PERIODS   0x40045030  /* _IOR('P',0x30,4) */
#define AKPCM_IOC_SET_MODE      0x000050e1  /* _IO('P',0xe1) */
#define AKPCM_IOC_RESET_BUF     0x000050e3  /* _IO('P',0xe3) */

/* Playback side (/dev/pcmC0D0p), shared by the 1080p and the 2K: the same numbers on both drivers. Notes on each call are in
 * platforms/anyka-ak3918/ak_ioctls.h; on the 2K they were read from libplat_osal.so (osal_ao_set_param, osal_ao_set_source,
 * osal_ao_start_playing, osal_ao_enable_speaker, osal_ao_set_gain) and match the 1080p capture. */
#define AKPCM_IOC_PREPARE       AKPCM_IOC_GET_PARS     /* 0x401c50e0, 28-byte struct akpcm_pars */
#define AKPCM_IOC_RESUME        AKPCM_IOC_SET_MODE     /* 0x000050e1, no argument */
#define AKPCM_IOC_SET_SOURCES   0x40045010             /* _IOW('P',0x10,4), int32 (stock: 4 play, 0 idle) */
#define AKPCM_IOC_GET_SOURCES   0x80045010             /* _IOR('P',0x10,4) */
#define AKPCM_IOC_SET_GAIN      0x40045030             /* _IOW('P',0x30,4), int32 (stock: 2) */
#define AKPCM_IOC_GET_GAIN      0x80045030             /* _IOR('P',0x30,4) */
#define AKPCM_IOC_SET_SPEAKER   0x40045081             /* _IOW('P',0x81,4), int32 0/1 (stock: 1) */
#define AKPCM_IOC_GET_RATE      0x800450ea             /* _IOR('P',0xea,4), 8000 after PREPARE */

struct akpcm_pars {
    uint32_t format;
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    uint32_t period_size;
    uint32_t period_count;
    uint32_t reserved;
};

#endif
