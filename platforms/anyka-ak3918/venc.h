#pragma once

/* Clear stale VENC channel state left by a previously killed anyka_ipc.
 * Safe to call unconditionally — if channel is already clean it's a no-op. */
void venc_reset_stale_channel(void);

/* VENC_IOCTL_QUERY_DMA → CREATE_POOL (ring) → CREATE_POOL (ext) → mmap. */
int venc_create_pool(void);

/* VENC_IOCTL_CREATE_ENC — configure H.264 Main 1920×1080 encoder. */
int venc_create_encoder(void);

/* VENC_IOCTL_BIND_VI_CHN — bind encoder channel 0 to VI channel 0 (kernel mode). */
int venc_bind_vi(void);

/* VENC_IOCTL_ACTIVATE — start frame production. */
int venc_activate(void);

/* VENC_IOCTL_SET_QP on the running channel 0 (img.qp). 0 on success, -1 (errno set) when refused or no encoder yet.
 * The caller has validated 10 <= min <= max <= 51. Not hardware-tested on the 1080p. */
int venc_set_qp_live(int minqp, int maxqp);
