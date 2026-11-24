# Video latency of `/stream0`, LSC Rotatable 2K

Measured on the camera, 2026-11-23, before this work: about 2 s glass to glass on `/stream0` (2304x1296, 10 fps at night, 3000 kbit/s VBR, GOP 30, Wi-Fi -67 dBm; ffplay over RTSP/TCP with
`-fflags nobuffer -flags low_delay -framedrop`). Target: under about 500 ms. This page is the analysis from the code and the fix; **camera side: validated on 2026-11-23** (frame age at send avg about 5 ms, max about 20 ms; glass to glass improved, not yet measured with a stopwatch)
(checklist item L2K-1 in [hardware test checklists](../../hardware-test-checklists.md#2k-video-latency)). For a user: [the picture is late](../../troubleshooting.md#the-picture-is-late).

## Where a frame can wait (before the fix)

Path: `ak_vi_get_frame` -> `ak_venc_encode_frame` (synchronous) -> `a2k_stream_publish` (copy, `ak_venc_release_stream` at once) -> ring (`stream.c`, 6 frames) -> per-client cursor ->
`tx_pace_decide` -> `net_tx` private queue -> kernel socket buffer -> Wi-Fi. Estimates at 3 Mbit/s (375 kB/s):

| Place | Before | Delay it can add |
|---|---|---|
| Encoder output | copied and released in the capture thread, nothing is held | none (the encode itself is one frame time, about 60 to 100 ms; the VI keeps 2 or 3 frames of depth, not measurable offline) |
| Ring (`A2K_RING` 6) | a new client began at the newest keyframe *in the ring* whatever its age (up to 5 frames), a client behind the ring only learned it when more than 6 frames were lost | up to 0.6 s at 10 fps (0.4 s at 15 fps) of replayed backlog |
| Pacing (`tx_pace`) | never sleeps and adds no fixed delay, but a backlog is allowed up to 0.5 s of the bitrate plus the last keyframe (up to 256 KiB), and **nothing bounded the time** a backlog could stand | 190 kB + 150 kB = about 0.9 s on a link that carries a little less than the stream |
| Private queue (`net_tx`, 512 KiB main) | holds what the kernel does not take; counted in the backlog above, so bounded by it | part of the 0.9 s |
| Kernel send buffer | `SO_SNDBUF` 256 KiB (about 380 kB real; the pacing cap is 3/4 of it) | up to 1 s of stream standing in the kernel on a slow link |
| Client wake-up | the capture thread writes a wake pipe per frame, the client thread `poll`s it: no timer, no sleep | under 1 ms |
| RTCP, RTP clock | sender reports every few seconds; the RTP clock (`rtp_clock.c`) only stamps | none |

Together on a weak link: ring 0.4 to 0.6 s + a standing backlog 0.9 s + kernel buffer = about 2 s, which is what was seen.

## What changed

- **Join** (`a2k_stream_join`): a client starts at the newest keyframe only when it is at most `A2K_MAX_AGE_MS` (300 ms) old, else it starts at the head, the encoder is asked for a keyframe and
  frames are dropped until it arrives (one keyframe request per 300 ms is merged in `cap.c`, as before).
- **Catch-up** (`a2k_stream_catchup`, every pass of the client loop): when the next frame of a client is older than 300 ms, the client jumps to the newest keyframe that is itself younger than that,
  or drops everything and waits for a new keyframe. The queue latency of a slow reader is bounded by 300 ms plus one keyframe instead of by the ring. Logged as
  `client N frames behind (older than 300 ms), skipping to the newest keyframe`.
- **Latency bound in `tx_pace`** (shared with the 1080p): the time the unsent backlog stays above an idle level (50 ms of the stream rate, at least 8 KiB) is tracked; past 300 ms plus the time the
  latest keyframe takes at the nominal rate, the client drops to the next keyframe. A healthy link never reaches it (the backlog empties after each keyframe), a link that is a little too slow
  no longer builds a permanent queue. Pacing itself still never delays a frame: it either writes at once or drops (no sleep, no fixed hold-back).
- **`SO_SNDBUF` 64 KiB** (`TX_PACE_SNDBUF`, both cameras, was 256 KiB): the kernel doubles it; the private queue takes the keyframe burst and the pacing counts both. About 50 Mbit/s at 20 ms round trip fits.
- The encoder was already drained promptly (copy, release, no client can hold it); not changed.
- **Frame age in the log**: every 10 s per playing client, `stream0: frame age at send avg A ms max M ms over N frames, unsent backlog max B bytes`. The age is the time from the encoder output
  (`rx_us`, taken in `a2k_stream_publish`) to the moment the last packet of the frame was handed to the socket or the queue, so it is the camera's own share: ring, pacing, writing. Add
  `B / bitrate` for the time the backlog still needs on the wire. The 1080p prints the same line without the ring (its age is only the writing, the encoder ring is read the moment a frame is ready).

## Expected result

On a link that carries the stream: no added delay (the keyframe's own serialisation, 0.2 to 0.4 s at 2 to 1x the rate, as the host simulation shows). On a link that does not: the picture
drops to the next keyframe (a freeze of up to one GOP, 3 s at GOP 30 and 10 fps, shorter when a keyframe is requested) instead of a standing delay. A lower GOP or bitrate helps the keyframe burst.

## Tests

`tests/rot2k_latency_test.sh` (virtual time, fake encoder, slow reader): bounded queue latency at 2x, 1.15x and 0.6x the stream rate (and not growing over ten minutes), a joining client gets the
newest recent keyframe, a client that stalled for 2 s or was starved for 1.5 s is back to current frames, pacing adds no steady delay, and the statistics. `tests/tx_pace_test.sh` covers the latency bound alone.

## Still to measure on the camera

Glass-to-glass with a stopwatch (a phone clock in front of the camera, ffplay beside it); the camera-side frame age from the log line; the VI depth contribution; the effect of the ffplay/VLC receive buffer.
