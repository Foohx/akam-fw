# Two-way audio: talk through the camera speaker

*What this page is for: speaking through the camera from your computer or from Home Assistant, Frigate or go2rtc.
You need it only if you want sound to come out of the camera.*

Akam can play your voice, or an audio file, on the camera's speaker.

It uses the standard **ONVIF audio backchannel**. ONVIF is the standard that NVRs and apps use to talk to cameras;
the backchannel is its way to send sound to the camera. It runs on the RTSP port (554, the camera's video port), so
there is no extra port. Any client that speaks the standard (go2rtc, Frigate through go2rtc, Home Assistant's WebRTC
card, some NVRs) can talk to the camera. The
design is in [specs/audio.md](../specs/audio.md), step 1.

**The Rotatable 2K** has the same feature, with the same settings (`audio.*`, see [2K API](porting/lsc-rotatable-2k/control-api.md)): the speaker is **off by default** there
(`akam audio on`, or `audio.out_enable 1` through the options screen or the signed API, turns it on at once), and the backchannel works over TCP only
(go2rtc and ffmpeg ask for TCP by default). `akam talk`, `akam say` and `akam audio` work on both cameras. There are no Home Assistant entities for the speaker on the 2K yet.

**Status.** 1080p: `talk` (the speaker) is validated on two cameras (see [testing status](testing-status.md)); `say`, the volume and the beep were not heard on a 1080p yet.
2K, validated on 2026-11-23: the beep, and talk with `tools/akam-talk` over TCP; go2rtc and Frigate were not checked, and the assistant's own `audio beep` and `say` on a 2K have not
run on a camera ([checks](hardware-test-checklists.md#2k-speaker-two-way-audio-and-picture-settings)). Please report what you hear.

## What you need

- The **video password** (`akam rtsp-auth set`). The camera **never offers talk without it**: anyone on
  your network could otherwise speak through the camera. There is no "open talk" mode.
- Audio output on (the default). The setting `audio.out_enable=1`, and `audio.talk_enable=1` (the default).

## From this computer

```bash
akam talk            # your microphone, until Ctrl-C
akam talk 30         # the same, at most 30 seconds
akam talk test       # a 2 second test tone: checks the whole chain without a microphone
akam say hello.mp3   # any audio file ffmpeg can read (a WAV at 8 kHz mono 16-bit needs no ffmpeg)
```

The native `akam` binary needs no external program for this on Linux and Windows: it decodes WAV, MP3, FLAC and Ogg
Vorbis files itself and captures the microphone itself (PulseAudio / PipeWire on Linux, WASAPI on Windows; macOS still
needs ffmpeg). See [the akam program](assistant.md). The bash wizard (`./akam` without the binary) captures the microphone with `ffmpeg` (PulseAudio or PipeWire) or `arecord`, converts it to
8 kHz mono and sends it with `akam-talk`, a small C program in [tools/akam-talk](../tools/akam-talk/)
(it is built once from source with your C compiler or the pinned Zig, or taken from
`tools/akam-talk/bin/<os>-<cpu>/`). The password is asked without echo (or read from
`AKAM_RTSP_PASSWORD`) and reaches `akam-talk` through its environment only: never a command line, the
state file or the log. `AKAM_TALK_CAPTURE_CMD` replaces the capture command (it must write raw 8 kHz mono
signed 16-bit little-endian audio on its standard output).

`akam-talk` can also be run by hand, for scripts:

```bash
AKAM_RTSP_PASSWORD=... akam-talk --tone 2 rtsp://viewer@<camera-ip>:554/
ffmpeg -i in.mp3 -ar 8000 -ac 1 -f s16le - | AKAM_RTSP_PASSWORD=... akam-talk --format s16le rtsp://viewer@<camera-ip>:554/
```

Exit codes: 0 done, 1 usage, 2 network, 3 the camera does not offer talk, 4 login refused, 5 protocol
error, 6 the input held no audio.

## From go2rtc, Frigate and Home Assistant

go2rtc asks every RTSP camera for the backchannel (`Require: www.onvif.org/ver20/backchannel`). Akam
answers it when the video password is on; without it Akam answers `551 Option not supported`, and go2rtc
falls back to a plain video stream, which is exactly what a camera without a speaker does. The addresses
below use the documentation address `192.0.2.10`: replace it, and URL-encode special characters of the
password (`@` = `%40`, `:` = `%3A`, `/` = `%2F`).

`go2rtc.yaml` (also the `go2rtc:` section of Frigate's config):

```yaml
streams:
  akam:
    - rtsp://viewer:PASSWORD@192.0.2.10:554/
  # for a browser: add an Opus copy of the camera's audio
  akam_web:
    - ffmpeg:akam#audio=opus
```

Frigate (recording from the go2rtc restream, so that go2rtc is the **only** client of the camera):

```yaml
cameras:
  akam:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/akam
          input_args: preset-rtsp-restream
          roles: [detect, record]
```

Home Assistant, with go2rtc (or the Frigate add-on) and the WebRTC Camera card:

```yaml
type: custom:webrtc-camera
url: akam
media: video,audio,microphone
```

The browser needs HTTPS for the microphone. Press and hold the microphone button to talk.
These snippets follow the go2rtc, Frigate and card documentation; they have **not been run against an
Akam camera yet**.

### One client at a time

The camera serves **one RTSP client at a time**, and this version does not change that (a second slot
costs a rewrite of the connection loop and is decided only after a hardware test). What is guaranteed:
a single RTSP connection can carry the video, the microphone and the talk track together, so a client that
adds the backchannel to its one connection works (go2rtc does this for its producer). What it means in
practice:

- Let **go2rtc be the only client of the camera**, and point Frigate, Home Assistant and your viewers at
  go2rtc (as in the Frigate example above). Do not also open the camera directly with another player or
  with Frigate's own ffmpeg: it would wait, or cut the other one.
- `akam talk` opens its own connection. While go2rtc is connected, it waits like any other second client.
  Stop the stream (or the go2rtc source) first, or run `akam talk` when nothing else watches.
- If a client that opens a separate connection just to talk is needed, report it: that is the case a second
  slot would solve.

## Half duplex, hiss and settings

The camera's microphone is replaced by silence **only while speech is playing** on the speaker (and for
300 ms after it): this prevents the screech of the speaker feeding the microphone, and it does not mute
the camera while a client keeps the talk channel open and sends silence, as go2rtc does. The video and
the audio timestamps are not affected. A stream of silence also does not switch the amplifier on, so no
hiss while nobody speaks.

Settings (`akam audio ...` or the options screens, through the signed API; by hand: port 8091 on the camera's own loopback, over SSH; saved in `/etc/config/akam/audio_out`):

| Key | Values | Meaning |
|---|---|---|
| `audio.talk_enable` | `1` (default), `0` | soft switch of talk (`0`: the camera refuses the backchannel with 551, and ends a talk in progress within a second) |
| `audio.talk_duplex` | `half` (default), `full` | `full` never mutes the microphone (use it with headphones elsewhere, or to hear the echo) |
| `audio.out_volume`, `audio.out_gain`, `audio.out_amp` | see [speaker output](troubleshooting.md#speaker-output-test-the-camera-speaker-by-ear) | volume and amplifier |

```bash
ssh root@<camera-ip> "printf 'SET audio.talk_duplex full\n' | nc -w 2 127.0.0.1 8091"
ssh root@<camera-ip> "printf 'LIST\n' | nc -w 2 127.0.0.1 8091 | grep -E 'audio\.(talk|out)_'"
```

Read-only state and counters on the same port: `audio.talk_state` (`idle` or `active`), `audio.talk_codec`
(`pcmu` or `pcma`), `audio.talk_packets`, `audio.talk_dropped`, `audio.talk_lost` (packets that never
arrived), `audio.talk_sessions`, `audio.talk_mic_muted`, and for the speaker buffer `audio.out_underruns`,
`audio.out_drops`, `audio.out_buf_ms`. During a good talk, `underruns` and `lost` stay at about 0.

## When it does not work

- **`akam talk` says the camera does not offer talk (exit 3):** the video password is off (`akam
  rtsp-auth status`), or `audio.out_enable=0` / `audio.talk_enable=0`, or the speaker failed to start
  (`audio.out_state` should be `idle`).
- **Login refused (exit 4):** the user or password is wrong; `akam rtsp-auth set` chooses new ones.
- **The camera does not answer (exit 2), or waits:** another client holds the camera, see above.
- **Silence, or only clicks:** check the speaker alone first with the beep (`akam talk test` sends a
  tone; the beep test is in the [speaker output](troubleshooting.md#speaker-output-test-the-camera-speaker-by-ear)
  section). Then watch `audio.talk_packets` rise while you speak.
- **Choppy voice:** `audio.out_underruns` rises: the Wi-Fi link drops packets or is late. Talk is carried
  in the RTSP TCP connection, so nothing is lost, but it can arrive late.
- **Voice too quiet:** raise `audio.out_volume` (default 100, the maximum) or the hardware `audio.out_gain`.

## Security

The password is a Digest login (as for the video): the audio itself is **not encrypted**, so someone on
the same network who can capture packets hears what is said. Do not expose port 554 to the Internet; use a
VPN. Failed logins are slowed down and the connection is closed after five. Talk follows the video
password: switching the password off, or choosing another one, ends a talk in progress within a second. So does
`audio.talk_enable=0`. UDP audio is accepted only from the address of the RTSP client.

## Cost on the camera

Measured on the build: the stripped `akamd` grows by about 13 KB (the project estimate was 8 to 15 KB).
The jitter buffer already existed. Expected CPU while talking, not measured yet on a camera: well under
0.5 % (about 50 packets a second, a table-free decode and an integer energy gate); nothing when nobody
talks.

A hands-on check list for whoever tests on a camera is in the [hardware checklists](hardware-test-checklists.md#talk-through-the-speaker-hands-on-1080p).
