# Video and streaming

`ak_rtsp` (`src/ak_rtsp`) captures the sensor through the vendor ISP and encoder, and serves one H.264 stream (plus
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

Only one encoder channel exists. The sensor's second and third video devices are opened but no sub-stream is encoded.
Resolution is fixed at 1920x1080 (the ISP crop, VI formats and the encoder DMA pool are sized for it). Mirror and flip
are not offered.

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
- Values never change while `ak_rtsp` runs. A change applies at the next start of `ak_rtsp`. The control port keys
  `video.bitrate_cfg`, `video.fps_cfg`, `video.gop_cfg` write the file (loopback only) and `video.cfg_pending` says
  whether the file differs from the running encoder.
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
s=ak_rtsp
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
