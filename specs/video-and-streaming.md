# Video and streaming

`akamd` (`platforms/anyka-ak3918`) captures the sensor through the vendor ISP and encoder, and serves one H.264 stream (plus
microphone audio) over RTSP on TCP port 554. This file covers the pipeline, the encoder settings, the RTSP server and
its optional Digest login.

Status: **hardware-validated** (VLC, ffmpeg, go2rtc over TCP; RTSP Digest login; 503 for a second client; frame rates
15 and 25 fps). Host-tested only: bitrate and GOP settings on the camera, UDP transport with real players.

## 1. Pipeline

1. `venc_reset_stale_channel`, then open the ISP and video (VI) devices (the encoder MUST NOT be open yet).
2. `isp_load_sensor_conf` programs the GC20C3 registers from `/tmp/sensor_isp.conf`; `isp_pipeline_init` sends the 24
   ISP modules (30 blocks) of that file unmodified, as the stock application does.
3. VI channel attributes, buffer request and capture start; then the encoder devices are opened, the DMA pool and the
   encoder are created, the encoder is bound to the VI channel, activated, and the stream is started.
4. The encoder output is fetched from a ring buffer and packetised for RTSP. The ring pool is 4,120,384 bytes
   (H.264 1080p with MMA); the ring does not wrap for about 1.5 s of video while a client takes its time between
   DESCRIBE and PLAY.

Only one encoder channel exists. The sensor's second and third video devices are opened but no sub-stream is encoded: on the 1080p
`/stream1` is the same picture as `/stream0`, on purpose (decision of 2026-11-23: a second encoder channel is not wanted here; the stock
firmware does create one at 640x360, see section 5.1, so it is possible but costs memory and CPU on a small SoC).
Resolution is fixed at 1920x1080 (the ISP crop, VI formats and the encoder DMA pool are sized for it). Mirror and flip
are offered since 2026-11-23 (`image.flip`, `image.mirror`, `image.ceiling`, section 5.7; not yet hardware-validated).

## 2. Encoder

| Parameter | Value |
|---|---|
| Codec, profile, level | H.264 Main (`profile_idc` 77), level 4.0 |
| Resolution | 1920x1080 |
| Frame rate | 15 fps default; 10, 15, 20 or 25 configurable. 30 is refused (see below) |
| Night frame rate | Always 10 fps (sensor frame time VTS 3426) |
| Rate control | VBR+ with a bitrate ceiling; QP range 28 to 43 (read-only) |
| Bitrate ceiling | 1536 kbit/s default; 500 to 3000 configurable |
| GOP (key-frame interval) | 2 s default (30 frames at 15 fps); 1 to 4 s configurable; the encoder receives `seconds x fps` frames |
| Packetisation | `packetization-mode=1`, FU-A fragmentation for large NAL units |

**Saved video settings** are read once at start from `/etc/config/akam/video` (mode 0600, key=value, LF, a trailing CR
is tolerated):

| Key | Values | Default |
|---|---|---|
| `bitrate` | whole number 500 to 3000 (kbit/s) | 1536 |
| `fps` | 10, 15, 20, 25 | 15 |
| `gop` | whole number 1 to 4 (seconds) | 2 |

- An absent file or key means the default. A bad line (unknown key, bad number, out of range) is skipped with one log
  line and its key keeps the default.
- The file holds only keys that differ from the default; with all defaults it is removed.
- Values never change while `akamd` runs. A change applies at the next start of `akamd`. The control port keys
  `video.bitrate_cfg`, `video.fps_cfg`, `video.gop_cfg` write the file (loopback only) and `video.cfg_pending` says
  whether the file differs from the running encoder.
- `minqp` / `maxqp` (10 to 51, defaults 28 / 43, `minqp < maxqp` required): the QP bounds of the VBR+ rate control. They
  go to both places of CREATE_ENC that carry them (`enc.qp_or_kbps`/`enc.qp2` at +0x28/+0x2a and, in the legacy rc
  sub-struct, `rc.minqp`/`rc.maxqp` at +0x44/+0x46; the stock rc sub-struct of `AKAM_VENC_RC=stock` keeps its 0/0). A
  pair that breaks the rule is dropped as a whole (both keys default, one log line); a SET that would break it is
  refused (`./akam video` sends maxqp first when raising both). Lowering `minqp` lets the encoder spend more bits on P
  frames (finer textures), which is the lever against key-frame pumping.
- What the SDK offers for the encoder quality, from `platforms/anyka-ak3918/ak_ioctls.h` (kernel ABI, byte-exact against the stock
  capture) and the 2K `libmpi_venc.so` analysis (`docs/porting/lsc-rotatable-2k/venc-live-params.md`; the 1080p uses the
  same kernel venc adapter, not proven for each ioctl):
  rc mode `enc.rc_mode`: 0 AVBR, 1 default, 2 CBR, 5 VBR+ (stock and ours: VBR+); changing it moves the meaning of
  `qp_or_kbps`/`qp2`, and one wrong rc struct has already oopsed the kernel (`venc.c`), so `rc` is NOT offered.
  QP: `minqp` 10..51, `maxqp` 10..62 (H.264 library limit 51), `initqp` (`initqp_vbrp`, stock 99, not offered).
  `rc.delta` (-12..+12, 0 today) exists in the struct, its meaning (I/P QP offset?) is not proven: not offered.
  `rc.I_pic_size/P_pic_size/B_pic_size` exist, all 0. Target and max bitrate are one field in VBR+ (`max_kbps_vbrp`).
  Separate I-frame QP bounds exist only as a live call in the 2K library (`ak_venc_set_iframe_param`, ioctl
  0x401056f9, 16 bytes, `IFrame_minqp/maxQP`); not in the CREATE_ENC struct.
  Live setters (2K library, same kernel ABI, UNTESTED on the 1080p and not used): `set_qp` ioctl 0x400c56fb `{id,min,max}`,
  `reset_qp` 0x400456fc, `set_rc_type` 0x400456c5, `set_attr` 0x406056f2. The 1080p applies QP at the next
  re-open of the encoder (video service restart), like bitrate and GOP.
- Sharpening: the picture setting `isp.sharpness` (-50..50, saved in `/etc/config/akam`, `./akam options` Picture) is
  already exposed (`isp_tune.c`); the finer ISP sharpening/edge-enhancement blocks are in the ISP conf the sensor
  driver copies to `/tmp/sensor_isp.conf` (see `isp_blocks.c`), a later step.
- The sensor frame rate is changed together with the encoder rate. Allowed pairs (frame time VTS = 34260 / fps):
  10 fps = 3426, 15 fps = 2284, 20 fps = 1713, 25 fps = 1370. The longest exposure equals the frame time, so a higher
  rate makes dim scenes darker and night mode start earlier.
- 30 fps (VTS 1142) was tested on a camera and is refused: the picture was dark, grainy and nearly grey. A RAM test
  delivered exactly 30 fps, but the setting is not offered.

**Timestamps.** The encoder stamps frames from a nominal clock; the real sensor rate differs by about 2 %. `rtp_clock.c`
therefore builds video and audio RTP timestamps on one monotonic microsecond base (video through a small PLL that
learns the real frame period; audio from the sample counter). RTCP Sender Reports pair the same instant with each
track's RTP timestamp. The video RTP clock is 90 kHz.

## 3. RTSP server

| Item | Value |
|---|---|
| Port | TCP 554 (`RTSP_SERVER_PORT`), listen backlog 5 |
| Library | SmolRTSP (pinned commit, MIT) for message parsing and RTP transports |
| URL | `rtsp://<ip>:554/`. The server does not check the path; any path reaches the same stream |
| Methods | `OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER`. `OPTIONS` also serves as keep-alive |
| Track ids | `streamid=0` video, `streamid=1` microphone audio, `streamid=2` ONVIF backchannel |
| Transports | TCP interleaved (`RTP/AVP/TCP;unicast;interleaved=a-b`) and UDP (`RTP/AVP/UDP;unicast;client_port=...`). Audio uses the same transport kind as video |
| Clients | One at a time |

### SDP

```
v=0
o=- <time> <time> IN IP4 0.0.0.0
s=akamd
t=0 0
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=XXXXXX;sprop-parameter-sets=<B64>,<B64>
a=control:streamid=0
m=audio 0 RTP/AVP 97              (omitted when audio.mic_enable=0)
a=rtpmap:97 L16/8000/1
a=control:streamid=1
m=audio 0 RTP/AVP 0 8             (only for a client that asks for the backchannel)
a=control:streamid=2
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=sendonly
```

- `profile-level-id` and `sprop-parameter-sets` come from the SPS/PPS cached from the live stream; they are omitted
  until both are known.
- The microphone track is raw 16-bit linear PCM, 8 kHz, mono (RFC 3551 L16, network byte order), payload type 97.
  It is not G.711.
- With `audio.mic_enable=0` the SDP has no audio section at all, because a track that never carries data makes some
  players wait for audio before showing video. Track ids do not change.
- The backchannel section is described in [audio.md](audio.md).

### Single client, 503 and pacing

- While a session runs, the listening socket joins the session's own `poll` set. A second client is answered at once
  with `RTSP/1.0 503 Service Unavailable`, `Retry-After: 10`, a `Reason` line ("one viewer at a time, restream with
  go2rtc for several") and `Connection: close`, then closed. At most 3 newcomers wait for their request at once (more
  are closed on accept); each is answered after 500 ms whether or not it sent a request. `CSeq` is echoed when found.
  The 503 costs no thread and does not disturb the running stream. For several viewers, restream with go2rtc.
- **Idle limits before PLAY** (a silent LAN host must not hold the only slot): the connection is dropped after 20 s
  without a complete request, and 60 s after connect if PLAY was not reached. While playing there is no idle drop; the
  stream ends on TEARDOWN, RTCP BYE, disconnect or a write stall.
- **TCP write policy.** The interleaved stream cannot survive a short write, so a write is never abandoned half way:
  it is retried until delivered or until the peer accepted not one byte for 15 s (`TX_STALL_BUDGET_MS`). `SO_SNDBUF` is
  set to 256 KiB. For TCP video, bytes the kernel does not take wait in a private 256 KiB queue per client (allocated
  at PLAY) drained with `POLLOUT`, so the encoder buffer is released as soon as the frame is serialised. A write that
  does not fit the queue fails and the client is dropped.
- **Backpressure pacing** (`tx_pace.c`). Before each video frame the unsent bytes (kernel backlog plus queue) are
  compared with a high watermark `min(base + last key-frame size, cap - frame_len)`, where `base = max(64 KiB, 0.5 s
  of the configured bitrate)` and `cap` is 3/4 of the socket send buffer plus the queue. Above it the stream enters
  SKIP mode: every frame is dropped (and released) until a key frame arrives while the backlog is below
  `base / 4`; that key frame is sent and the stream resumes. A P-frame is never sent after a dropped frame without a
  key frame in between. While skipping, the client is dropped when the backlog has not reached a new minimum for 15 s
  (dead peer) or the skip run exceeds `max(3 GOPs, 10 s)`. Dropped frames are counted (`rtsp.skipped_frames`).
- Enter and leave events are logged at most once per 10 s.

Status: hardware-validated (14.9 to 15 fps, TCP queue 0 to 24 KB, 0 encoder errors; 100 second-client attempts all
got 503 with the stream unaffected).

## 4. RTSP Digest authentication

Optional, **off by default**, managed only over SSH.

### File and states

`/etc/config/akam/rtsp_auth` (mode 0600, directory 0700), one line in `htdigest` form:

```
USER:akam:HA1        HA1 = md5hex("USER:akam:PASSWORD"), 32 lowercase hex digits
```

- `USER` is 1 to 32 characters from `A-Z a-z 0-9 . _ @ + -`. The realm is fixed: `akam`. A file with another realm is
  invalid.
- The file is re-read at every protected request and periodically during a stream, so a change takes effect without a
  restart (a running stream is cut within about a second when the change invalidates it). Writes are atomic.

| File state | Mode | Behaviour |
|---|---|---|
| absent | OFF | No authentication |
| present and valid | ON | Digest required |
| present but unreadable or invalid | BROKEN | Fail closed: every protected request is refused |

### Protocol

- Everything except `OPTIONS` is protected (DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, SET_PARAMETER, unknown
  methods). `OPTIONS` stays open as keep-alive and availability probe.
- Challenge: `401 Unauthorized`, `WWW-Authenticate: Digest realm="akam", nonce="<32 hex>", algorithm=MD5`, plus
  `stale=true` when the credentials are right but the nonce is old or unknown.
- No `qop` in the challenge: live555 (VLC) and go2rtc only implement the RFC 2069 form
  `response = MD5(HA1:nonce:MD5(method:uri))`. A client that answers with `qop=auth` still works: the RFC 2617 form is
  verified, with `nc` strictly increasing.
- Algorithm absent or `MD5` (case-insensitive); `MD5-sess`, `SHA-256` and others are refused.
- Nonce: 16 bytes from `getrandom()` (fallback `/dev/urandom`; failure means refuse), one per connection, valid for 120 s
  to authenticate. A header replayed on another connection fails.
- A connection that authenticated stays authenticated while the configuration is unchanged: later requests without an
  `Authorization` header are accepted (some clients omit it on TEARDOWN); a request with a header MUST verify.
- A change of the file (enable, new credentials) cuts a running stream within a second when the connection is no longer
  valid; disabling cuts nothing.
- User and response are compared in constant time (the hash is computed even for a wrong user). The `uri` in the header
  is hashed as sent. Each header parameter has a bounded length (refuse, never truncate).

### Failure limiter and logs

- A failure is a malformed header or a wrong response (not a missing header, not `stale`). One process-wide counter
  resets after 60 s without failure. Delay before the 401: failures 1 and 2: 0; 3: 500 ms; 4: 1 s; 5 and more: 2 s.
  Five failures on one connection close it. The limiter is shared with ONVIF (`cred.c`).
- Log lines `[ak_rtsp] auth: ...` give the reason, the counter and the client address. They never contain the typed
  user name, the header, the response, the password or the HA1.
- Known limits: with one client slot, a LAN host can retake the slot by reconnecting in a loop (network access control
  is the only real fix); Digest without `qop` does not protect against an active attacker on the path or eavesdropping
  on the stream; the HA1 in the file is equivalent to the password for Digest.

### Camera command `akam_rtsp_auth.sh`

Installed at `/usr/sbin/akam_rtsp_auth.sh`; run over SSH as root. POSIX sh for BusyBox 1.30.1.

| Command | Effect |
|---|---|
| `status` | `enabled (user NAME, ONVIF-ready \| not ONVIF-ready)`, `disabled`, or `INVALID` (exit 3) |
| `show` | `user: NAME` or `disabled` |
| `set USER` | Asks the password twice without echo (at least 8 characters), writes both files |
| `set-creds` | Reads two stdin lines (user, password) and writes both files. This is what the installer uses: the password is typed on the PC and never appears in `argv` |
| `set-hash` | Legacy. Reads one `USER:akam:HA1` line, writes only the htdigest file and removes the clear-text copy (ONVIF is then not ready) |
| `disable` | Removes both files |

Password length is 8 to 128 characters. The second file, `/etc/config/akam/credentials` (`user=NAME`, `pass=PASSWORD`,
mode 0600), exists because ONVIF WS-Security needs the clear text. `set`, `set-creds` and `disable` keep both files in
step. The control port never reads or writes either file.

Exit status: 0 ok, 1 failure, 2 usage, 3 invalid file.

## 5. Image-quality controls and stock runtime parity (1080p, research of 2026-11-16)

Sources: the stock 1080p userspace (`anyka_ipc`, from the camera backup mtdblock7), `ak_isp.ko`, `ak_venc_adapter.ko`,
`ak_venc_bridge.ko` and `isp_gc20c3.conf` of the image, the stock capture `stock_capture_cam3/run02` (330 s with a cover and
uncover of the lens, ISP ioctl statistics per command), the 2K `libmpi_venc` notes
(`docs/porting/lsc-rotatable-2k/venc-live-params.md`, same vendor family, same ioctl numbers). Evidence: **capture** = seen
on the wire, **RE** = read in the disassembly, **doc** = in our notes, **guess**. Nothing here was run on a camera.

### 5.1 What the stock userspace does at run time that we do not: nothing found

Stock capture, ISP calls per block over 330 s (a gain change happened: lens covered at 60 s, uncovered at 180 s): every
block (BLC, LSC, NR1, NR2, UVNR, 3D_NR, DEMO, RGB_GAMMA, CCM, FCS, WDR, SHARP_EX, SATURATION, CONTRAST, DPC, LCE, AF, WB,
AWB_EX, Y_GAMMA, HUE) was sent **3 times**: at start and at the two day/night switches (SHARP 5 and EFFECT 9: the switches
also send the effect type). The rest is reading: `GET_EXPOSURE` 3916, `GET_3D_NR_STAT` 401 (1.2 Hz), `GET_SENSOR_FPS` 636.
So the userspace does **not** re-send NR, sharpness, demosaic or gamma according to the gain.

The ISO linkage lives in `ak_isp.ko` (RE): `_get_iso()` turns the current gain product (analogue x digital x ISP gain) into a
level with thresholds copied from the exposure block; `_isp_para_change_with_envi()` re-writes SHARP, NR1, NR2, 3D NR (+rc),
BLC, CCM, HUE and the AWB state for that level. It is called from the per-frame `_isp_3dnr_work` (which also consumes the
3DNR statistics itself: `_get_3dnr_stat` has no other caller) and from `ak_isp_ae_fast_set_ae`.
`ak_isp_updata_param_with_envi_work` is an empty stub. The tables it reads are inside the blocks we send byte for byte as
stock (`specs/image-day-night-leds.md` section 1). AE and AWB are kernel work too (`aec_work`, `ak_isp_awb_work`,
`_awb_ex_ctrl`); `ae.c` only polls like stock (`aec_custom.c`, a userspace AE, is a build option that is off).

| Stock at run time | We | Evidence | Gap / fix |
|---|---|---|---|
| ISO-linked NR / sharp / demosaic / BLC / CCM / hue | kernel, same blocks | capture (3 sends), RE (`_isp_para_change_with_envi`) | none. The suspicion "we run with static NR" is not supported |
| `isp.sharpness` slider | rescales group 0 of the SHARP block only | RE of the block: 17 entries of 0xab4 bytes, entry 0 = manual (unused, mode 1), entries 1..16 = ISO levels 0..15 (section 5.5.1) | **gap in our slider**: it only touches the manual entry, so it does nothing. `img.sharp_all=1` rescales entries 0..15 (misses level 15) | Since 2026-11-16 the default is sharpness -15 on the entries that matter (section 5.6).
| AE (kernel) polled at ~12 Hz, one SET_EXPOSURE per mode | same | capture | none |
| AWB: SET_WB, AWB_EX, AWB_CALIB per mode | same (`img.wb=conf`, default) | capture | none; `img.wb=manual` deviates on purpose |
| 3DNR statistics and sensor fps reads | off by default (`img.poll=0`) | capture, RE (the kernel reads its own stats) | none expected; `img.poll=1` is the control experiment |
| AF, DPC, LSC | sent at start and switches only, in both | capture (no AF reads) | none (fixed focus) |
| Flip/mirror block 0x40044979 sent once (zeros) | sent only when a flip or mirror is on (saved file or a SET), see section 5.7 | capture, RE | none (default no flip: nothing is sent) |
| 3D NR reference size `{1920,1080}`, sensor fps | sent (`ae.c`) | capture | none |
| Second encoder channel 640x360 | absent | capture (CREATE_ENC #2) | none for quality |
| CREATE_ENC rc sub-struct: all zero but `flag=1` | `legacy`: minqp/maxqp 28/43, srd 128, flag2 40; `stock` option exists | capture vs `venc.c` | `venc_rc=stock` was tried on hardware: no gain (`imgq.h`) |

Conclusion: there is no run-time userspace linkage to copy. If the picture is worse than stock it is either the same behaviour
(stock uses the same 1536 kbit/s, QP 28..43, GOP 30) or an effect of what we change: the encoder side (section 5.2) and the
sharpness slider scope. The quality work is the encoder and the kernel tables, not a missing loop.

### 5.2 Control inventory

Exposed = a control-port key exists (SET from loopback, or debug mode). "Live" = no encoder restart.

| Control | Ioctl / place, layout | Range | Live? | Evidence | Crash risk | Exposed |
|---|---|---|---|---|---|---|
| Min / max QP, open time | CREATE_ENC `enc.qp_or_kbps` +0x24, `qp2` +0x26 and rc `minqp` +0x44, `maxqp` +0x46 | 10..51, min < max | no (restart) | capture | low (in use) | `video.minqp_cfg`, `video.maxqp_cfg`, `./akam video minqp N maxqp N` |
| Min / max QP, running | `VENC_IOCTL_SET_QP` 0x400c56fb, 12 B `{chn, min, max}` | 10..51, min <= max | yes | RE (kernel handler copies 12 B, constant present in the 1080p dispatch); the 2K lib validates the same and it returned 0 on the 2K | low-medium: never run on the 1080p; try a no-op first (`img.qp 28 43`) | `img.qp` (new) |
| Bitrate cap, GOP, fps | CREATE_ENC `max_kbps_vbrp` +0x28, `goplen` +0x1a, `fps` +0x18 | 500..3000, 1..4 s, 10..25 | no (restart) | capture | none (in use) | `video.*_cfg` |
| I/P QP offset | CREATE_ENC `rc.delta` +0x48 (s32), kernel log "uIpDelta", `AL_Encoder_SetQPIPDelta` | -12..12 | no (open) | doc (ak_ioctls.h) + RE strings; meaning of 0 unknown | medium: the rc sub-struct crashed the kernel twice for other fields (flag 0 / zero minqp) | start-up option `ipdelta` (new, default 0, RAM file first) |
| I-frame QP bounds | `ak_venc_set_iframe_param` 0x401056f9, 16 B, present in the 1080p kernel | 10..51 style | yes | doc (2K, MEDIUM): arguments not pinned | unknown layout | **no** |
| Reset QP, current QP, rc stats | 0x400456fc, 0x400456c8, 0x400856c6 | - | yes | doc (2K) | read-only ones low | no (the reads could help a trial) |
| rc type CBR/VBR, rc param | 0x400456c5, 0x400256c7 (struct starts with a u16 QP) | - | yes | doc (2K, size unknown) | unknown layout | **no** |
| Generic live attributes | `venc_set_attr` 0x406056f2 (the 96 B CREATE_ENC image) | as CREATE_ENC | yes | doc (2K: bitrate and GOP worked live) | medium: re-runs the validator | no (the restart path is enough) |
| Smart / static scene | CREATE_ENC `smart_mode` +0x34, goplen, quality, static value; kernel "AKV Smart params" | mode 0 = off | no (open) | RE strings, modes unknown | unknown | **no** |
| ROI / motion-detect quality (`bRoiEnable, uMdThreshold, uQuality, uRecoverStep, uWaitSkip, uWaitIntra`) | `venc_set_ROI_Mdinfo`, ioctl number not located | - | yes | RE strings | unknown layout | **no**. Off by default (the param is NULL until set) |
| Skip / static region (SRD), MMA, MVV range | rc `srd_threshold` +0x5d, `flag` +0x58, `flag2` +0x5c (module parameter `MVVRange_factor`) | 1..254 | no (open) | doc + RE strings | high (the two crashes) | **no** |
| Deblocking offsets | `AL_Encoder_SetLoopFilter{Beta,Tc}Offset` exist in the kernel library | - | - | RE symbols only, no ioctl found | unknown | **no** |
| Long-term reference / GDR (intra refresh) | `AL_*LongTerm`, `AL_IsGdrEnabled`, "AdvanceSkipFrameReferenceParas" | - | - | RE symbols only | unknown | **no** |
| Force an I frame | `venc_request_idr` 0x400456f3 | - | yes | doc (2K, used) | none | not needed |
| ISP sharpness slider | SHARP 0x40044925, 46584 B, kernel interpolates per ISO | -50..50 | yes | capture + RE | low (same block, clamped) | `isp.sharpness`; `img.sharp_all` (new) widens it to all 16 groups |
| Brightness, contrast, saturation | EXPOSURE block, CONTRAST, SATURATION blocks | -50..50 | yes | capture | low | `isp.*` |
| 2D NR (NR1 3064 B, NR2 992 B, UVNR 720 B) | 0x4004490c, 0x4004492b, 0x4004497d; the kernel re-writes them per ISO | strengths inside per-level tables | yes (SET any time) | capture (block bytes), RE (linkage); layouts decoded in section 5.5 | medium: a bad table goes to the ISP registers | **no** (keys specified in 5.5.7) |
| 3D / temporal NR | 3D_NR 0x4004492f, 7076 conf bytes but the kernel copies 7180 (section 5.5.1) + ref size 0x40044967 + stats 0x608 B; per-frame kernel work | section 5.5.5 | yes | capture, RE | medium | **no** (keys specified in 5.5.7) |
| Demosaic, GB, FCS, DPC, gamma (Y 302 B, RGB 1746 B), LCE 1500 B, WDR 16936 B, RAW_LUT, BLC, LSC, CCM, HUE | one ioctl each (`isp_blocks.c` table) | tables | yes | capture | medium | **no** |
| Exposure block gain limits | SET_EXPOSURE 0x8004498a, 1824 B: day sensor gain 1.0x, ISP gain 1.5x (night 3.0x) | - | yes | capture | medium | `ae.*` covers the AE tuning words only |
| `en_drop_by_3dnr` (ak_isp.ko, a /proc entry under the umap directory) | write "0" or "1", a flag read per frame | 0/1 | yes | RE: exact path and default not verified | low-medium | no |
| Module parameters | `ak_venc_adapter`: `MVVRange_factor`, `UseExtStreamBuf`, `sched_priority`; `ak_isp`: `flip_drop_frames`, `chgfps_drop_frames`, ... | - | insmod only | RE strings | - | no |

### 5.3 Keys added (all default to the previous behaviour)

- `img.qp`: `SET img.qp MIN MAX`, live QP bounds (section 5.2). RAM only. `GET` says `unchanged` until a first accepted SET.
- `img.sharp_all`: 1 (default since 2026-11-16, section 5.6) or 0, the sharpness slider on all 16 SHARP groups. RAM only; a non-zero slider is re-applied at once.
- start-up option `ipdelta` (-12..12, default 0; env `AKAM_VENC_IPDELTA`, `/tmp/akam_image` as `ipdelta=N`, or the saved
  file): `rc.delta` of the legacy CREATE_ENC rc sub-struct. 0 keeps the payload byte for byte; `tests/video_apply_test.sh`
  proves it, and that otherwise only the 4 bytes at +0x48 change.
- `./akam video minqp N maxqp N` (the bash path now has what the Go assistant had).
- `tools/video_quality.sh`: static-region metric (sharpness drop from the I frame to the last quarter of the GOP, luma drift to
  the I frame, I-boundary jump).

### 5.4 Camera trial plan (one variable at a time, safest first)

Scene fixed, camera still, same light. Capture 20 s each: `ffmpeg -rtsp_transport tcp -i rtsp://CAM:554/ -t 20 -c copy tN.mkv`, then
`tools/video_quality.sh tN.mkv`. Control-port commands from the camera: `printf 'SET key value\n' | nc -w 2 127.0.0.1 8091`.
Between trials restore the default. Read per trial: P avg size and bitrate, "sharpness drop" (I to last quarter), the
I-boundary jump ratios, "drift", and a still crop by eye.

| # | Command | Restart | Expected | Metric / what to look for |
|---|---|---|---|---|
| 0 | none, twice | no | noise floor of the metric | spread between the two captures |
| 1 | `SET isp.sharpness -40` (entry 0 only) | no | nothing at most gains (previous measurement) | sharpness mean unchanged |
| 2 | `SET img.sharp_all 1` (slider still -40; this is now the default, section 5.6) | no | sharpness mean down, halos weaker, a smaller I/P sharpness gap | sharpness mean and drop; if unchanged, the SHARP groups are not what limits it. Back: `SET img.sharp_all 0`, `SET isp.sharpness 0` |
| 3 | `SET img.poll 1` | no | no change (control: stock polls the 3DNR statistics) | all metrics equal to #0 |
| 4 | `SET img.qp 28 43` | no | accepted, nothing changes. A refusal or a stall here ends the live-QP path (`ERR ... refused by the encoder`) | answer line, stream still plays 30 s, kernel log |
| 5 | `SET img.qp 24 43`, then `22 43` | no | P frames larger (8 KB to 12+ KB), bitrate up to the 1536 cap, smaller sharpness drop, steadier textures | P avg size, bitrate, drift to the I frame. If the bitrate sticks at the cap, do #6 first |
| 6 | `./akam video bitrate 2500` (restart) | yes | more room for #5; the QP floor is reached more often | bitrate, P size |
| 7 | `./akam video gop 1` (restart) | yes | I every 1 s: the drift ceiling halves, the snap is twice as frequent but smaller | drift at the last quarter, I/P ratio |
| 8 | `./akam video minqp 22 maxqp 43` (restart), the persistent form of #5 | yes | as #5 | same |
| 9 | `ipdelta=-3`, then `ipdelta=3`, in `/tmp/akam_image`, restart | yes | the I/P QP gap changes: one sign shrinks the I/P size ratio (less pumping), sign unknown | I/P size ratio, sharpness drop. RISK: rc sub-struct; a kernel oops reboots the camera into the flashed image (the RAM file is gone); have the UART or be ready to power cycle. Do not save the option |

Not to run (unknown layout, crash risk): `set_iframe_param`, `set_rc_param`, `set_rc_type`, ROI/MD, smart mode, rc
`flag`/`flag2`/`srd_threshold`, deblocking, long-term reference, and every NR/SHARP field that section 5.5 leaves
"unknown". The NR1/NR2/UVNR/3D_NR/SHARP layouts are decoded in section 5.5, with the edits it allows (5.5.6) and their
own trial plan (5.5.8).

### 5.5 Noise reduction and sharpening tables: decoded layouts (research of 2026-11-16)

Sources: `ak_isp.ko` of the 1080p image (not stripped; `_set_nr1_attr`, `_set_nr2_attr`, `_set_uvnr_attr`,
`_set_3d_nr_attr`, `_set_3d_nr_rc_attr`, `_set_sharp_attr`, `_isp_para_change_with_envi`, `_get_iso`, the ioctl handler
in `isp_param_file_open`), `anyka_ipc` (`isp_set_attr`), the 3 subfiles of `isp_gc20c3.conf` (day, night, and a third
with mode byte 2). No Anyka SDK header was found (none in the repo, the backups or the 2K appfs, whose libraries only
carry function names). Evidence: **RE** = read in the disassembly, **conf** = value pattern in the 3 subfiles,
**slider** = what the stock sharpness slider does to the field (the stock `isp_set_effect` code already ported in
`isp.c`), **guess**. Nothing here was run on a camera. The layouts are in `platforms/anyka-ak3918/isp_nr_layout.h` with static
asserts (not yet included by any source file).

#### 5.5.1 Outer shape and ISO level selection

| Fact | Evidence |
|---|---|
| NR1, NR2, UVNR, 3D_NR, SHARP (and GB, FCS, WDR, LCE, SATURATION) are `u32 mode` + 17 entries: entry 0 = manual, entry `1 + L` = ISO level `L` (0..15) | RE: the per-level case table of `_isp_para_change_with_envi` (level 0 points at entry 1, level 15 at entry 16; mode 0 selects entry 0). Sizes: NR1 4+17x180, NR2 4+17x58(+2), UVNR 4+17x42(+2), 3D_NR 4+17x416, SHARP 4+17x0xab4, WDR 4+17x996, LCE 4+17x88 |
| The mode word is 1 (linkage) for NR1, NR2, UVNR, 3D_NR, SHARP, WDR, LCE in all 3 subfiles; GB and FCS are 0 (manual) | conf |
| One entry per level, no interpolation between levels; a few 3D_NR fields are smoothed over frames when the level changes (factor in 1/16) | RE |
| Level = `_get_iso()`: `G = ((w4 * w8) >> 8) * w12 >> 8` from the AE run info (`GET_AE_RUN_INFO` 0x80044952, words at +4, +8, +12, i.e. the `hw_exp`, `sgain`, `isp` of `GET night.ae`), compared with 16 pairs `{lo, hi}` (u32, scaled by 256) at +0x98 of the exposure block (SET_EXPOSURE, 1824 B): first pair with `lo <= G < hi`, else 15. Special case: when exposure word +0x08 != 0 and run-info word +16 < exposure word +0x3c, G = 0 (level 0) | RE (run info = ctx+0x1c64c, exposure block = ctx+0x1bf2c, both consistent with every load in `_get_iso`). The fall-through for empty pairs is approximate |
| Day and subfile 2 pairs: [0,2) [2,4) [4,8) ... [512,1024), levels 10..15 empty ([1024,1024)). Night: [0,2) [2,4) [4,8) [8,16) [16,24) [24,32) [32,48) [48,64) [64,128) [128,256) [256,512) [512,1024) [1024,1536) [1536,2048) [2048,2560) [2560,4096). Day exposure +0x08 = 1, +0x3c = 342 | conf |
| G is an exposure index (exposure word times gains), not a pure gain: `hw_exp` goes up to 16384. Which level daylight uses is therefore **not known**: anywhere from 0 (bright scene, or the +0x3c special case) to about 8 | RE + guess; trial step 1 (5.5.8) settles it |

Consequences for the existing keys and the start-up path (RE, visible without hardware):

- `isp.sharpness` rescales entry 0 only, which is the **manual** entry: with mode 1 the kernel never uses it, so the
  slider has no effect in any subfile. This matches the measured "-40 on group 0: nothing" (section 5.4 step 1).
- `img.sharp_all=1` rescales entries 0..15 = manual + levels 0..14: **level 15 (entry 16) is missed** (`SHARP_GROUPS`
  should be 17; entries 1..16 are the ones the kernel uses).
- **3D_NR size mismatch.** The kernel copies **7180** bytes for SET and GET 0x4004492f / 0x80044930
  (`copy_from_user(..., 0x1c0c)`, RE), not 7076: after the 17 entries it reads 17 x 6 bytes of a "3D NR rc" table
  (`_set_3d_nr_rc_attr`: entry `+0` bit 0 -> 3D NR register 0x20 bit 19, byte `+2` and byte `+4` -> register 0x7c bits
  0..15), selected with the same mode word and level. Stock (`anyka_ipc isp_set_attr`, RE) memsets a 0x1c0c struct and
  copies the 7076 conf bytes into it: **stock sends zeros** for that table. `isp_send_modules()` passes a pointer into a
  7080-byte malloc'd module buffer, so the kernel reads **104 bytes past its end (heap contents)**: today the rc enable
  bit and register 0x7c take undefined values, which may change from boot to boot and differ from stock. Every other
  block's copy size equals ours (RE: all `ak_isp_vp_set_*` call sites of the ioctl handler checked). `img.dump` is safe
  (64 KiB buffer).

#### 5.5.2 The Gaussian weight tables (shared by the NR blocks)

NR1 (3 tables), NR2, UVNR, 3D_NR and SHARP carry 17-point tables generated by the vendor tool from a strength `s`
stored next to them (the kernel never reads `s`):

`lut[i] = max(floor, round(1023 * exp(-256 * i^2 / s)))`, `i = 0..15`; `lut[16]` is the floor or 0 (kept as is).

Checked against all 354 tables with `s > 0` of the 3 subfiles: every value within +-1 LSB (conf; the vendor's exact
rounding is not reproduced). The 3 tables with `s = 0` (night SHARP) are all ones and must not be regenerated.
`floor` is the minimum of `lut[1..15]` (1, 10, 20, 50 or 100 in the files). The values are 10-bit (RE: packed with
`<< 22 >> 22`, three per register). Read as a range filter: weight of a neighbour whose difference is `i` steps; a
larger `s` keeps weight for larger differences = **stronger smoothing**. The vendor raises `s` with the level (NR2:
200, 500, 800 ... 6000; UVNR 100 ... 8800; NR1 100 ... 13000).

#### 5.5.3 NR1 (0x4004490c, 3064 B, entry 180 B), NR2 (0x4004492b, 992 B, entry 58 B), UVNR (0x4004497d, 720 B, entry 42 B)

| Block | Offset | Width | Values (levels 0 -> 15, day) | Meaning | Evidence |
|---|---|---|---|---|---|
| NR1 | +0x00 | 1 bit | 1 (0 in the manual entry) | enable | RE |
| NR1 | +0x02 | 4 bit | 4, 5 | unknown | RE |
| NR1 | +0x04 | 5 bit | 31, 28, 20 ... 12, 0 | unknown, falls with the level | RE + conf |
| NR1 | +0x06, +0x08, +0x0a | 16 bit | 100 ... 13000 (equal) | `s` of the 3 weight tables | conf (not read by the kernel) |
| NR1 | +0x0c | 17 x 10 bit | 0, 64 ... 1023 (linear, or concave at low levels) | intensity-dependent strength curve | RE (fields), meaning guess |
| NR1 | +0x2e | 16 x u16 | 0, 5, 16 ... | tool data | RE (never read) |
| NR1 | +0x4e, +0x70, +0x92 | 3 x 17 x 10 bit | Gaussian, identical | range weights (luma 2D NR strength) | RE + conf |
| NR2 | +0x00 / +0x02 | 1 bit / 1 bit | 1 / 0..1 | enable / unknown | RE |
| NR2 | +0x04, +0x06, +0x08, +0x0a | 4, 5, 4, 4 bit | 4..5, 31/12/0, 15, 15 | unknown | RE |
| NR2 | +0x0c | 16 bit | 200 ... 6000 | `s` | conf |
| NR2 | +0x0e | 17 x 10 bit, [0] not read | Gaussian | range weights | RE + conf |
| NR2 | +0x30, +0x34, +0x36, +0x38 / +0x32 | 1 bit / 10 bit | 0..1 / 50, 30, 20 | unknown | RE |
| UVNR | +0x00 / +0x02 / +0x04 | 1 / 1 / 4 bit | 1 / 0..1 / 2..4 | enable / unknown / unknown | RE |
| UVNR | +0x06 | 16 bit | 100 ... 8800 | `s` | conf |
| UVNR | +0x08 | 17 x 10 bit, [0] not read | Gaussian | chroma range weights | RE + conf |

**Live NR1 block of both 1080p cameras (akam-01, akam-02, `SET img.dump nr`, 2026-11-16; fixtures
`tests/fixtures/isp_nr_dump_akam-0{1,2}.txt`).** It differs from the conf pattern above, and the first decode refused it
("live table not as decoded", `GET img.nr` showing `NR1 unreadable`): the **mode word reads 192, not 1**; the 16 linkage entries
are all zero (enable 0, `s = 0/0/0`), only the manual entry has `s = 64/64/64`. The kernel tests the mode word for non-zero only
(5.5.1), and an entry with `s = 0` is skipped by every composer, so the decode now accepts 192 for NR1 (`isp_nr.c: mode_ok`);
any other mode word, for any block, is still refused. NR1 is therefore a no-op for `img.nr2d` and `img.nr2d_bias` on these
cameras (disabled in every linkage entry as read). NR2 (mode 1, `s` 200 ... 6000), UVNR (mode 1, 100 ... 8800) and 3D_NR (mode 1,
`s` 50 ... 20000, lo/hi 800/1800 ... 7000/8000) are as decoded. The dump holds decoded fields and the block hash only (NR1
`d20fa646`, NR2 `57566d32`, UVNR `db994035`, 3D_NR `1e25cdfe`), not the table bytes, which stay private vendor data; the table
check of NR2/UVNR on the real bytes is the one thing that only a camera run proves (`SET img.nr2d 150` answering OK).

#### 5.5.4 SHARP (0x40044925, 46584 B, entry 0xab4 = 2740 B)

| Offset (entry) | Width | Values (levels 0 -> 15, day) | Meaning | Evidence |
|---|---|---|---|---|
| +0x000 | u16 | 1 | enable | conf |
| +0x018 / +0x03a | 17 x u16 / u16 | Gaussian (floor 50 or 1) / `s` 50 ... 500 | pre-sharpen denoise weights | conf (generator) |
| +0x03c..+0x63a | | zeros at the day levels | unknown | conf |
| +0x63e, +0x64c, +0x65a, +0x664 | u16, <= 15 | 15, 15, 10..0, 10..0 | gains (slider q4) | slider |
| +0x646, +0x64a, +0x654, +0x658, +0x65e, +0x662, +0x668, +0x66c | u16, <= 255 | 30..16 (first 4), 16..8 (last 4), falling with the level | gains (slider q8) | slider + conf |
| **+0x644, +0x648, +0x652, +0x656, +0x65c, +0x660, +0x666, +0x66a** | u16, <= 255 | 4 (levels 0..6), 6, 8, then 10 at level 15; 8, then 15 at level 15 | **coring threshold**: the stock slider lowers them when it raises the sharpness, the vendor raises them as noise grows | slider + conf (meaning: strong guess) |
| +0x66e, +0x670, +0x672 | u16 | 511/128, 20..40, 0..15 | unknown | conf |
| +0x674, +0x874 | 2 x 256 x s16 | S-shaped: negative (min about -60) below index about 128, a near-zero band around it, positive (max about +56) above, rolling off at both ends | detail transfer curves (dead band = coring, roll-off = overshoot limit); the slider scales them with `kernel_gain` | slider + conf (shape); input scale and centre: guess |
| +0xa74, +0xa92 | 9 x u16 each | 0, 20, 54 ... 255 | unknown curves | conf |

No threshold beyond the 8 fields above is proven; editing the dead band of the 256-entry curves is not proposed until a
live dump confirms their input scale.

#### 5.5.5 3D_NR (0x4004492f, entry 416 B; 7180 B on the ioctl)

| Offset | Width | Values (levels 0 -> 15, day) | Meaning | Evidence |
|---|---|---|---|---|
| +0x000 / +0x002 / +0x004 | 1 / 1 / 2 bit | 1 / 1 / 0..2 | enable / unknown / unknown | RE |
| **+0x006 / +0x008** | 13 bit each | 800/1800, 900/1800, 1000/2000, 1500/2500, 1800/2800 ... 5800/6800, 7000/8000 | **motion ramp**: below `lo` a pixel counts as static (full temporal blend), above `hi` as moving; the kernel computes `65536 / (hi - lo)` as an 8-bit slope and keeps `hi > lo`. Rising with the level = "noise is not motion" | RE (arithmetic) + conf; meaning: strong guess |
| +0x00c | 128 x 4 bit | 3..8 | per-luma table | RE (fields), meaning guess |
| +0x10e | 16 x u16 | 0, 8, 16 ... 128 | rising curve | conf, use unknown |
| +0x12e | 4 bit | 10, 10, 8, 6, 5, 5, 0 ... | unknown | RE |
| +0x130 / +0x132 | u16 / 17 x 10 bit | `s` 50 ... 20000 / Gaussian | weights of the 3D NR filter (temporal difference or spatial: not proven) | RE + conf |
| +0x154..+0x18f | two parallel groups of 4..10-bit fields (`15, 10..15, 80..260, 32/64, 0, 0, 76..120, 20..40, 4..6, 6..12, 8..10, 1..4, 32..120`) | | unknown (Y and UV temporal parameters?) | RE (masks), meaning unknown |
| +0x190..+0x198 | u16 | 3..6, 0..10, 0..15, 0, 100..150 | unknown | conf |

#### 5.5.6 What can be changed safely, and what not

Proven enough to edit (values stay inside what the vendor files or the stock slider already produce):

1. **Level bias** (any block): for each level `L`, copy the vendor entry of level `min(L + n, 15)` of the same block and
   subfile into entry `1 + L`. Every byte sent is a vendor byte that the kernel already runs at some exposure. Safest.
2. **Gaussian strength** of NR1 (3 tables), NR2, UVNR: regenerate the tables with `s' = s * p / 100` (formula of
   5.5.2), keeping `lut[0]`, `lut[16]` and the floor; rewrite `s` too (unused by the kernel, kept consistent). Values
   stay 0..1023.
3. **SHARP coring**: multiply the 8 coring thresholds by `p / 100`, clamp 255 (the stock slider's clamp; at slider -50
   the stock code doubles them).
4. **3D NR motion ramp**: multiply `lo` and `hi` by `p / 100`, clamp 8191 (13 bit), keep `hi >= lo + 1`. Medium: the
   arithmetic is RE, the meaning a strong guess.
5. **3D NR rc tail**: send the 7180-byte struct with a zeroed tail, as stock does.

Not to edit (layout or meaning not proven): every "unknown" field above, the NR1 intensity curve, the 3D_NR luma table
and parallel groups, the SHARP transfer curves (beyond the slider's existing scaling), the manual entries, the mode words.

#### 5.5.7 Implementation spec for the control keys (implemented: `isp_nr.c`, `isp_nr_ctl.c`, `isp.c`, `imgq.c`; run on akam-01/02, 5.6)

All `img.*` keys: RAM only (never saved; a restart or a reboot returns to the vendor bytes; `isp.denoise` is the saved front end of
`img.nr3d_bias`, 5.6), 8091 SET from loopback only (same
rule as the other `img.*` keys), refused with `not_ready()` before `isp_pipeline_ready()`, run under `isp_cfg_lock()`.
Defaults leave the blocks untouched: **no ioctl is sent while every key of a block is at its default**, and returning a
key to its default re-sends the pristine baseline byte for byte. No rc struct change, no new start-up option.

| Key | Range (default) | Blocks re-sent | Bytes written (linkage entries 1..16 only; entry 0 and the mode word never) |
|---|---|---|---|
| `img.nr2d_bias` | 0..4 (0) | NR1, NR2 | entry `1+L` <- vendor entry `1+min(L+n,15)`, whole entry |
| `img.uvnr_bias` | 0..4 (0) | UVNR | same |
| `img.nr3d_bias` | 0..4 (0) | 3D_NR | same, the 416-byte entries only (tail as `img.nr3d_rc`) |
| `img.sharp_bias` | 0..4 (0) | SHARP | same, whole 0xab4 entry, applied before the slider and the coring |
| `img.nr2d` | 50..400 % (100) | NR1, NR2 | NR1 +0x06/+0x08/+0x0a and the tables at +0x4e/+0x70/+0x92; NR2 +0x0c and the +0x0e table; formula of 5.5.2 on top of the bias; entries with `s = 0` skipped |
| `img.uvnr` | 50..400 % (100) | UVNR | +0x06 and the +0x08 table |
| `img.sharp_coring` | 100..300 % (100) | SHARP | the 8 coring fields, `min(255, v * p / 100)`, on top of the bias |
| `img.nr3d_motion` | 50..200 % (100) | 3D_NR | +0x06 `lo`, +0x08 `hi`: `min(8191, v * p / 100)`, then `hi = max(hi, lo + 1)` |
| ~~`img.nr3d_rc`~~ | not implemented | | Moot since `feature/1080p-sharp-default`: start-up and every reload already send the 7180-byte buffer with the zero tail (stock), so there is no heap tail to switch away from. The knob code keeps whatever tail the live GET returns (zero), so editing a table never changes it |
| `img.nr` | read-only GET | none | one line: the knob values, the live `G` and level (5.5.1, from `GET_AE_RUN_INFO` + `GET_EXPOSURE`), and that level's NR1 `s`, NR2 `s`, UVNR `s`, 3D_NR `lo/hi/s`, SHARP coring and q8 gains, read from the live blocks |
| `img.dump nr` | new value of the existing write-only key | none | logs, per block, the mode and the decoded proven fields of all 17 entries, from live GETs |

Implementation rules:

- One pure, host-testable composer per block (e.g. `isp_nr_compose(block, baseline, len, knobs, out)`) in a new file,
  not in `isp.c`, using `isp_nr_layout.h`. It validates the baseline first (length, mode word 1, and for every entry it
  would regenerate the `s`/table pair within +-1 of the formula) and refuses on any mismatch (returns -1, nothing
  sent, answer `ERR img.KEY: live table not as decoded`).
- Baselines: GET each block once at first use (3D_NR into a **7180-byte** buffer, the others at their table size; a
  64 KiB scratch like `img.dump` is fine) and cache them. `isp_reset_picture_baselines()` (day/night switch,
  `img.reload`) drops them and re-applies the non-default knobs on the fresh ones, like the picture sliders.
- SHARP keeps one sender: extend `apply_sharpness()` so the order is baseline -> `img.sharp_bias` -> slider scaling
  (entry 0, or entries 0..16 with `img.sharp_all`) -> `img.sharp_coring` -> one SET. Fix `SHARP_GROUPS` to 17 in the
  same change. Coordinate with the branch that changes the sharpness default (it touches the same function).
- Every 3D_NR SET from the new code uses a 7180-byte buffer: the 104 bytes that follow the 7076 are those of the GET
  baseline (zero, as start-up sends them), so editing a table does not change the tail. The composer refuses any other length.
- Each SET answers `OK img.KEY=V (RAM only, BLOCK re-sent)` or `ERR` with the reason; each GET returns the value;
  `LIST` shows the keys; `specs/control-port.md` lists them.
- Host tests (new `tests/native/isp_nr_test.c` and a `tests/*_test.sh` wrapper, group native) on a day subfile built by
  the test (or the synthetic conf of `isp_blocks_test`): (a) every knob at its default -> composer output identical
  (`memcmp`) to the baseline, and a fake tunnel sees **zero** NR/SHARP SETs; (b) each knob changes only its declared
  offsets (diff mask); (c) bias `n` copies vendor entry `L+n` byte for byte and clamps at level 15; (d) regenerated
  tables at 100 % reproduce the vendor tables within +-1 and stay within 0..1023 at 50 and 400 %; (e) coring and motion
  clamp at 255 and 8191, `hi > lo`; (f) refusal on a baseline whose mode word or `s`/table pair does not match;
  (g) back to defaults re-sends the baseline byte for byte; (h) 3D_NR SET length 7180, the tail kept (zero). Implemented in `tests/native/isp_nr_test.c`
  (`tests/isp_nr_test.sh`) against a fake driver that holds vendor-like tables built from the formula, plus the reload
  and the SHARP order (bias, slider -15, coring checked value by value). The golden repro hash changed (new code, default
  behaviour identical): `tests/golden/lsc-indoor-1080p-ak3918.sha256`.
- Final syntax: `SET img.nr2d_bias N`, `img.uvnr_bias N`, `img.nr3d_bias N`, `img.sharp_bias N` (0..4), `img.nr2d P`,
  `img.uvnr P` (50..400), `img.sharp_coring P` (100..300), `img.nr3d_motion P` (50..200); answers `OK img.KEY=V (RAM only,
  BLOCK re-sent)`, `... default: nothing sent` or `ERR img.KEY: live table not as decoded ...`; `GET img.KEY`; `GET img.nr`
  (`img.nr=G=<index> level=<0..15> knobs=<8 values in the order of the table above> NR1.s=... NR2.s=... UVNR.s=... 3D.lo=...
  3D.hi=... 3D.s=... SHARP.coring=<8> q8=<8>`); `SET img.dump nr` (log lines `[isp.nr]`, one per block and entry).
- Level selection (`img.nr`, informational only; no knob depends on it): `G` is computed in 32-bit like the kernel and
  compared with the pair words **as stored** in the exposure block. 5.5.1 says the pairs are "scaled by 256": if trial step 1
  shows a level that disagrees with the table of 5.5.1, the unit of the pairs is the thing to fix (one line in `isp_nr_level`).
- The knobs apply to entries 1..16 of the live blocks. A SET is refused (and the knob keeps its old value) when any entry it
  would rewrite fails the check; a bias alone only needs the length and mode word 1.

#### 5.5.8 Camera trial plan (coordinator, RAM only on akam-01)

Same capture and metric as 5.4: same scene, still camera, same light; 20 s per trial, `tools/video_quality.sh`
(static-region P-frame size, static sharpness, I-boundary sharpness jump, drift); visual check: centre crop of the last P
frame before an I frame, side by side with step 0. Restore every key to its default between steps (`SET img.KEY` to its default value puts the original bytes back; `SET img.reload 1` also
does, and drops all of them); one variable per step. The sharpness slider stays at the default -15, the knobs ride on it. **Abort** on any kernel oops or a stream that stops for more than 5 s: the camera reboots into the flashed image
(the RAM keys are gone, nothing to undo). Also stop at any `ERR ... live table not as decoded`: the live tables differ
from this decode; send the dump for analysis.

| # | Command(s) | Expected | Read / continue if |
|---|---|---|---|
| 0 | none, twice | metric noise floor | as 5.4 |
| 1 | **Read only.** `GET night.ae` 5 times 2 s apart, `SET img.dump all`, copy `/tmp/isp_live/isp_blocks.bin`; also `GET img.nr` and `SET img.dump nr` (log lines `[isp.nr]`) | NR1, NR2, UVNR, SHARP, 3D_NR say `day=eq`; in the bin every block starts with mode 1 and its entries decode as in 5.5.3..5.5.5; `G` and the level from `night.ae` | decode confirmed; note the **daylight level** (which entries matter) |
| 2 | (dropped: the zero rc tail is the default since `feature/1080p-sharp-default`) | | |
| 3 | `SET img.nr2d_bias 1`, then `2` | luma 2D NR of the next levels: smaller P frames, lower static sharpness, steadier textures | P size and drift down; stop at the first value that visibly smears fine texture |
| 4 | `SET img.nr3d_bias 1`, then `2` | stronger temporal NR: smaller P frames, less flicker at the I boundary; risk of trails on motion | P size, I-boundary jump; wave a hand to check trails |
| 5 | `SET img.sharp_coring 150`, then `200` | flat areas no longer sharpened (noise not amplified), edges unchanged | static P size and sharpness drop; crop: edges as step 0, flat walls cleaner |
| 6 | `SET img.nr2d 150`, then `200` | as 3, continuous | as 3 |
| 7 | `SET img.nr3d_motion 150` | more pixels blended over time, more trails | as 4 |
| 8 | `SET img.uvnr 200` | less chroma noise; colours of small objects may bleed | crop by eye |
| 9 | `SET img.sharp_bias 1`, then `2` | the softer sharpening tables of higher levels | compare with the previous `isp.sharpness -15` + `img.sharp_all 1` |
| 10 | best of 3..9 combined (e.g. `SET img.nr2d_bias 1`, `SET img.nr3d_bias 1`, `SET img.sharp_coring 150`) | | candidate default; repeat on a dim indoor scene (another level) and at night before any default change |

Expected direction: P frames of a static scene from about 8 KB of noise towards a few KB, a smaller sharpness jump at
each I frame, without the edge loss that sharpness -25 gave. A default change is a separate decision after step 10.

### 5.6 Results: sharpness default -15 on all ISO levels, 3D-NR bias 2 default (akam-01, akam-02, 2026-11-16)

#### 5.6.1 Sharpness -15 (akam-01)

RAM trials, static scene, `tools/video_quality.sh`, `SET img.sharp_all 1` then the slider. The stock SHARP block amplifies the sensor
noise, which the encoder then has to code in every P frame.

| `isp.sharpness` (entries 0..15 measured) | P frame average | I-boundary sharpness jump | Look |
|---|---|---|---|
| stock (0, entry 0 only) | 7.9 KB | 2.42 | noisy, crawling grain |
| -15 | 1.8 KB | 2.13 | keeps the knit texture |
| -25 | 0.56 KB | 0.88 | too soft |
| -40 | 0.30 KB | 0.18 | far too soft |

Decision (user, 2026-11-16): **-15 on all SHARP entries is the 1080p default** (`isp.sharpness` -15, `img.sharp_all` 1). The stock
behaviour stays one setting away: `SET isp.sharpness 0` (saved) or `./akam picture sharpness 0`, and `SET img.sharp_all 0`.
The trials ran with entries 0..15; the shipped code also covers entry 16 (ISO level 15, section 5.5.1), which the trials missed. The golden reproducible hash was re-recorded for it (tests/golden/lsc-indoor-1080p-ak3918.sha256).

#### 5.6.2 Second round on both 1080p cameras: sharpness vs 3D-NR bias (akam-01, akam-02, 2026-11-16)

Lamp light, ISO level 9, RAM only, medians of 3 interleaved 12 s A/B rounds (A and B alternate within a round). "Jump" = sharpness
jump at the I-frame boundary (what the eye sees as a pulse once per GOP); "drop" = sharpness lost over one GOP; "static" = sharpness of
a static scene (detail kept).

| A/B | Jump akam-01 | Jump akam-02 | GOP drop akam-01 | GOP drop akam-02 | Static sharpness | Verdict |
|---|---|---|---|---|---|---|
| `isp.sharpness` 0 -> -15 | 14.1 -> 7.6 | 3.4 -> 2.4 | 23.6 -> 19.0 % | 5.4 -> 4.8 % | (the -15 of 5.6.1) | confirms -15 on a second camera |
| `img.nr3d_bias` 0 -> 2 | 7.4 -> 5.2 | 2.5 -> 1.9 | 18.6 -> 14.5 % | 4.9 -> 3.8 % | -7 % / -3 % | **shipped as default** |
| `img.sharp_coring` 200 | mixed | mixed | mixed | mixed | | rejected (better on one measure, worse on another) |
| `img.nr3d_motion` 150 | no effect | no effect | | | | not shipped |
| `img.uvnr` 200 | no effect | no effect | | | | not shipped |
| `img.sharp_bias` 1 | no effect | no effect | | | | not shipped |

The bias is the one knob that moves both the I-boundary pulse and the GOP drop on both cameras, for a small loss of static detail (3 to 7 %).

Conclusions:

* **Decision (user, 2026-11-16): ship `isp.sharpness` -15 (all 17 SHARP entries) and `img.nr3d_bias` 2 as the 1080p defaults.** The 3D-NR
  bias is exposed as `isp.denoise` (0..4, default 2; `./akam picture denoise N`), a saved picture value like the sharpness: a saved
  value wins, `0` is the stock 3D-NR tables. It is applied once the ISP runs and again after every day/night reload (the reload
  puts the vendor blocks back; `isp_nr_reapply` re-sends 3D_NR only, with level L taking vendor entry min(L+2,15)). `img.nr3d_bias`
  stays as the RAM-only experiment key on the same knob.
* The safety rule is unchanged: when the live 3D_NR block is not as decoded (mode word not 1, an enable word that is not a bit,
  wrong length) nothing is sent. At start-up the refusal is logged (`[control] denoise 2 not applied (live table not as decoded): the
  camera keeps the stock noise-reduction tables`), the camera keeps running with the stock tables, and the saved or default value is
  not lost; `SET isp.denoise` answers `ERR ... live table not as decoded`.
* Stock path: `isp.sharpness` 0 and `isp.denoise` 0 send exactly the payloads of the previous release (no GET, no SET of 3D_NR;
  `tests/isp_nr_test.sh`, `tests/isp_effects_test.sh`). The golden hash changed (behaviour change, `tests/golden/lsc-indoor-1080p-ak3918.sha256`).
* Not made a default: `img.nr2d`, `img.nr2d_bias` (NR1 is empty on these cameras, 5.5.3; not A/B-tested), coring, motion, UVNR strength, sharp bias.

### 5.7 Flip and mirror (reverse engineering of 2026-11-23, not yet hardware-validated on the 1080p)

Settings `image.flip` (vertical), `image.mirror` (horizontal), `image.ceiling` (both): the same names, meaning and 0/1 values as the 2K
(`src/features/orient/orient_rows.h`). Live, no stream restart. Saved in `/etc/config/akam/orient` (`flip=1`, `mirror=1`), written only when
the user changes a value, removed when both are off, applied once at start-up before the video channel starts (`platforms/anyka-ak3918/image_orient.c`).

**Ways considered, in the order asked, and the one chosen**

1. Vendor SDK call: none on the 1080p. The stock `anyka_ipc` and our `akamd` talk to the kernel module directly (raw ioctls on `/dev/isp-param-0`);
   the AV130 library `ak_vpss_switch_flip_mirror` of the 2K ends in the same ISP ioctl. The kernel interface IS the SDK call, so this is the first
   way and the second at once.
2. Sensor registers through the driver/ISP interface: this is what the kernel does. ISP tunnel command `0x40044979` (`ISP_INNER_FLIP_MIRROR`,
   the `FLIP_MIRROR` of the stock capture, sent once with zeros by the stock) goes to `isp_param_file_open` of `ak_isp.ko`, which copies **8 bytes**
   from the payload (the command number encodes 4): two 32-bit words, word 0 = flip, word 1 = mirror, any non-zero is 1. It stores them in the ISP
   context (+0x954 and +0x955) and calls `collective_set_flip_mirror`, which (module parameter `sensor0_flip_mirror_enable`, default 1) calls
   `sensor_set_flip_mirror`: the flags `flip | mirror << 1` go to the sensor driver as control `0x3004`. `sensor_gc20c3.ko`
   (`gc20c3_core_s_ctrl`, case `0x3004`) writes the GC20C3 registers: register `0x022c` = `(flip << 1) | mirror` (page / bank select of the sensor
   as the driver addresses it), and, when the sensor is not in its initial state (a driver flag), the start-position corrections: register `0x0347`
   = its table value minus 2 when the picture is flipped, register `0x0b0d` = its table value minus 1 when mirrored (the two registers move the
   readout window by a whole Bayer pair so the colour order survives). `ak_isp` then updates its
   misc attribute (Bayer pattern of the VI) with the capture paused for the change (`flip_drop_frames`, default 1 frame dropped). So flip = vertical
   readout (bit 1 of the sensor register), mirror = horizontal (bit 0), as the 2K.
3. Flip in the encoder or VI path: not needed.

**Chosen: way 2.** The sensor does the flip while it reads out, so it costs no CPU and no memory in `akamd` (a software flip of a 1080p frame
would have cost a full frame copy per frame), and every consumer sees the same picture (RTSP, ONVIF snapshots if any, the AE statistics). Cost in
the binary: about 1.5 KB of code, no thread, no buffer. Source: disassembly of `ak_isp.ko` (functions `isp_param_file_open` case `0x40044979`,
`collective_set_flip_mirror`, `sensor_set_flip_mirror`, `camera_set_flip_mirror`) and `sensor_gc20c3.ko` (`gc20c3_core_s_ctrl`) of the stock
firmware `app` partition; nothing of it is stored in the repository.

**What is not known without the camera**

- The exact register meaning and values written by `0x3004` (decoded from a disassembly of the ARM code, not observed on the I2C bus, no GC20C3 datasheet at hand). The numbers above are the reading of the code.
- Whether the colours stay right after a flip (the Bayer order change is the kernel's job; it is the first thing to look at).
- Whether the kernel keeps the orientation across a day / night switch: the call is sent again after each switch (`image_orient_replay_locked`, under the ISP configuration lock the switch already holds; nothing is sent while both are off; the kernel ignores an unchanged value), so it should; the log line after the switch proves it.
- Whether sending the call before the video channel starts is accepted (the stock does the same, once, in its ISP set-up).
- The encoder may lose a frame or two while the sensor turns; the stream does not restart.
