# Audio

Akam captures the microphone into the RTSP stream, drives the speaker, and accepts two-way audio from standard clients
through the ONVIF RTSP audio backchannel on the same RTSP port. This file covers all three, the safety guards around the
vendor audio driver, and the `akam-talk` host client.

Status: **hardware-validated**: microphone track exists and plays, speaker output, backchannel talk with go2rtc-style
clients on two cameras (password required). **host-tested only**: microphone supervisor with backoff, `audio.mic_enable=0`
on a camera, `akam say` for MP3, FLAC and Ogg files on a camera.

## 1. Microphone capture

| Item | Value |
|---|---|
| Device | `/dev/pcmC0D0c` (vendor `ak_pcm`) |
| Format | 8000 Hz, 16-bit signed, mono |
| Period | 256 samples (512 bytes, 32 ms) per `read()`; every real read is exactly one period |
| Thread | One capture thread; periods go into a ring of 8 slots drained without blocking by the RTSP client loop |
| RTSP track | Raw L16 (linear PCM, network byte order), payload type 97, `streamid=1` (see [video-and-streaming.md](video-and-streaming.md)) |
| Timestamp | RTP timestamp = capture sample index, so a gap caused by a dropped period is kept |
| Failure | Never fatal: video continues without audio |

**Supervisor.** After 5 consecutive read errors the device is closed and the whole open-and-configure sequence is
retried after 1, 2, 4, 8, 16, 32 and then 60 seconds. A failing syscall MUST NOT be retried faster than 1 s: an earlier
100 Hz retry loop rebooted a camera. `audio_mic_restarts()` counts recoveries.

**Disabling.** `audio.mic_enable=0` (saved in `audio_out`) means the capture device is never opened and the SDP has no
audio section. It applies at the next start of `ak_rtsp`.

## 2. Speaker output

The playback device `/dev/pcmC0D0p` can only be opened once, and `ak_rtsp` owns it for its whole life. When audio out is
enabled `audio_out.c` runs the stock vendor sequence and a playback thread; when it is disabled or its sequence fails,
`audio.c` keeps a legacy silence-writing loop and nothing else touches the device.

| Item | Value |
|---|---|
| Format | 8000 Hz, 16-bit, mono |
| Period | 1024 bytes = 512 frames = 64 ms; 16 driver periods (about 1 s) |
| Device sequence | `SET_SOURCES`, `SET_SPEAKER`, `PREPARE`, `RESUME`, `GET_RATE`. `RESUME` MUST NOT run before a successful `PREPARE` |
| Content of a period | Silence, the test beep, or pushed samples |
| Test beep | Three 200 ms tones with 200 ms gaps, exactly 1 s |
| Volume | 0 to 100, applied as `(v/100)^2` in Q15 with a 10 ms ramp. Default **100** |
| Hardware gain | 0 to 5, default 2 (stock value) |
| Route source | 4 (default, stock) or 6 (experimental) |
| Amplifier | `amp=auto` (default): on while audible audio plays, off after 2000 ms of silence; `on`; `off` |
| Audible | A pushed period counts as audible only when its peak is at least 48 (about -56 dBFS). A client that streams G.711 silence all day MUST NOT keep the amplifier on |
| States | `off`, `idle`, `talking`, `test`, `chime` (reserved), `disabled_crash`, `error` |
| Failure | 5 consecutive write failures stop the thread (`error`) |

**Crash guard.** Before the first device call `ak_rtsp` creates `/etc/config/akam/audio_out.trying`. It is removed after
5 s of clean playback, and also after a sequence that failed cleanly (an error, not a crash). If the marker exists at
the next start, `ak_rtsp` died during audio initialisation: the sequence is skipped (state `disabled_crash`), the legacy
silence loop runs, and the log says to remove the file to retry. This prevents a reboot loop. On `SIGTERM`/`SIGINT` the
amplifier is switched off with an async-signal-safe ioctl.

**Saved settings.** `/etc/config/akam/audio_out`, key=value lines, atomic write, mode 0600, directory 0700, foreign lines
are kept:

| Key | Values | Default |
|---|---|---|
| `enable` | 0 or 1 | 1 |
| `volume` | 0 to 100 | 100 |
| `gain` | 0 to 5 | 2 |
| `source` | 4 or 6 | 4 |
| `talk_enable` | 0 or 1 | 1 |
| `talk_duplex` | `half` or `full` | `half` |
| `mic_enable` | 0 or 1 | 1 |

An absent file, an unknown line or a bad value keeps the default. A change is saved only when the value changed. A
SET that does not come from loopback (a remote peer in debug mode, MQTT, ONVIF) changes the running value only and
never reaches flash, and a RAM-only volume is not made permanent by a later write of another key.

## 3. Jitter buffer

`audio_jitter.c`: fixed memory (no `malloc`), one producer (the talk session) and one consumer (the playback thread).
At 8 kHz mono S16: pre-fill 120 ms (960 samples) before anything plays, capacity 400 ms (3200 samples). An underrun
fills the rest of the period with silence, counts it and re-arms the pre-fill. An overrun drops the oldest samples down
to the pre-fill level (this also corrects clock drift) and counts them.

## 4. Two-way audio (ONVIF RTSP backchannel)

Standard clients (go2rtc, Frigate, Home Assistant WebRTC cards, ONVIF viewers) send audio to the speaker on the RTSP
port, following the ONVIF Streaming Specification "Audio backchannel".

### Offer rules

- A client asks with `Require: www.onvif.org/ver20/backchannel` in `DESCRIBE`.
- The camera offers talk only when **the RTSP password is ON** and the speaker side can serve it (audio out enabled,
  device running, `audio.talk_enable=1`, state neither `error` nor `disabled_crash`). There is no open talk mode.
- When the client requires the backchannel and talk cannot be offered, the answer is `551 Option not supported` with
  `Unsupported: www.onvif.org/ver20/backchannel`. Clients that always send `Require` (go2rtc) retry without it.
- A client that does not send `Require` gets exactly the SDP of [video-and-streaming.md](video-and-streaming.md)
  (byte-identical to a camera without the feature).
- The extra SDP section is `m=audio 0 RTP/AVP 0 8`, `a=sendonly`, `streamid=2`, PCMU (payload 0) first, then PCMA
  (payload 8).

### Transport

`SETUP` of `streamid=2` accepts TCP interleaved (RTP and RTCP channels, which MUST differ) or UDP (`client_port`; the
camera binds a local RTP port and an RTCP port so the client's RTCP is not refused). `PLAY` starts the session. A session
ends on `TEARDOWN`, RTCP BYE or the end of the connection; the speaker plays out what is queued, then the amplifier
idles off. The `SETUP` stays valid, so the connection may still stream video.

### RTP input (`rtp_in.c`)

Every packet is bounds-checked (RFC 3550: version 2, CSRC list, header extension, padding) and nothing trusts a length
field. Only PCMU and PCMA are decoded (one byte per sample). A packet is dropped and counted by reason when it is:

| Reason | Condition |
|---|---|
| short | Shorter than the fixed header, or than its CSRC list or extension |
| big | Over 1400 bytes |
| version | Not RTP version 2 |
| padding | Padding length larger than the payload |
| payload type | Other than 0 or 8 (telephone-event, comfort noise, video) |
| empty | No payload |
| duplicate | Same or older sequence number than the last accepted |

A sequence jump of more than 1000 packets (20 s of audio) is taken as a stream restart, not as loss. Gaps are counted
as `lost`.

### One talker, idle limit

One talker at a time holds the speaker's push side; a second talker is refused. A talk-only session (it sends audio and
receives nothing) is dropped after 30 s without an RTP packet, so a silent talker cannot hold the single RTSP slot. A
talker that stops has its queued tail flushed after 250 ms of silence (the jitter buffer plays nothing below its
pre-fill level).

### Half-duplex rule

`audio.talk_duplex` is `half` (default) or `full`. In `half` mode the microphone samples in the outgoing RTSP stream are
replaced by silence while speech is playing on the speaker, to stop echo and feedback. Only the samples change: packet
count, length and RTP timestamps continue.

The gate (`audio_vad.c`) asks "is speech being played now?", not "is a talker connected?", because clients may keep the
channel open and send silence for hours. The decoded audio is cut into 20 ms windows (160 samples); a window with mean
square above 33955 (-45 dBFS RMS) is speech. Speech keeps the gate closed for 300 ms plus the time the jitter buffer
still delays that audio. Silent windows never close it. `full` never mutes the microphone.

### Counters

`audio.talk_*` keys on the control port ([control-port.md](control-port.md)) report state, codec, accepted and dropped
packets, lost packets, sessions since boot and whether the microphone is muted.

## 5. `akam-talk` host client

`tools/akam-talk/akam_talk.c`: portable C99 (Linux, macOS, Windows through winsock), MIT, reuses `md5.c` and `g711.c`.

```
akam-talk [options] rtsp://USER@HOST[:PORT]/PATH
```

- One TCP connection, like go2rtc: `DESCRIBE` with the Require header (Digest login), `SETUP` of the send-only track
  (RTP interleaved), `PLAY`, a G.711 RTP packet every 20 ms, then RTCP BYE and `TEARDOWN`.
- Input: stdin or a file (raw 8 kHz mono S16LE, a WAV with that format, or already G.711), or a generated test tone.
- The password comes from `AKAM_RTSP_PASSWORD` or `--password-file`; never from the command line, never printed.
- Exit codes: 0 done, 1 usage, 2 network, 3 camera refuses talk (551 or no backchannel in the SDP), 4 login refused,
  5 protocol error, 6 input held no audio.

The installers' `talk` and `say` commands use the same protocol. The Go assistant has it built in (G.711, WAV, and on
Linux and Windows MP3, FLAC and Ogg Vorbis decoding, microphone capture through PulseAudio/PipeWire or WASAPI); the bash
wizard calls `akam-talk` and needs `ffmpeg` or `arecord`.

## 6. Tests

`tests/audio_out_test.sh`, `audio_sup_test.sh`, `audio_vad_test.sh`, `jitter_test.sh`, `g711_test.sh`,
`rtp_in_test.sh`, `talk_test.sh`, `rtsp_backchannel_test.sh`, `rtsp_sdp_test.sh`, `akam_talk_test.sh`,
`wizard_talk_test.sh`, `wizard_audio_test.sh`.
