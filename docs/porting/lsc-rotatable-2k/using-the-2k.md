# Using the 2K after the install

*What this page is for: the first things to do once your Rotatable 2K runs Akam from its flash: stream password, MQTT and
Home Assistant, ONVIF, the settings file, the signed API. Skip it if you only want to watch the video.*

The 2K is configured through small files in `/config/akam` (the jffs2 partition, kept across Akam updates), written over
SSH, and through the signed API on port 8092. The `akam` program sets all of this on a 2K too: `led`, `picture`, `video`,
`hostname`, `track`, `wifi`, `settings`, and `rtsp-auth`, `mqtt`, `onvif`, `ntp`, `audio`, `talk`, `say` through the signed API (section 5, an
admin key made by `akam key-2k`), and the Camera options screen. At install time `prepare-2k-sd` asks for the same services and writes the
files on the first-install card (section 6). The 1080p-only commands are refused on a 2K (`backup`, `restore`, `verify`, `image`, `flash`,
`get-kit`, `config`, `key`, `control-port`: "not yet supported for this model"). The 2K runs a build from 2026-11-23 (before the
`akamd` rename). What ran on the real camera, row by row: [testing status](../../testing-status.md#lsc-rotatable-2k); the assistant's own 2K
commands are host-tested only, the API rows they use were checked on the camera.

Log in with the key you gave `prepare-2k-sd` (`ssh -i KEY root@CAMERA_IP`). Files are small: the partition is 256 KiB. Each
file is written only when you change it; Akam never rewrites them while running.

| File in `/config/akam` | What it does | When a change applies |
|---|---|---|
| `rtsp.conf` | the stream password (Digest); absent = no password | at the next request |
| `mqtt`, `mqtt_pass` | MQTT and Home Assistant | next restart of the app |
| `akam.conf` | options of the app: bit rates, ONVIF, microphone, tracking, LEDs, ... | next restart of the app |
| `zones`, `ptz.conf`, `ntp`, `hostname`, `net` | detection zones, PTZ, clock sync, name, Wi-Fi band | see their pages |
| `settings`, `api_keys` | settings saved by the API (including flip, mirror, ceiling), API keys | written by the app |

To restart the app without a power cycle: `killall akamd` over SSH; the supervisor starts it again within a few
seconds (it gives up after 5 exits in 10 minutes). The `akam` program never reboots the camera.

## 1. Set the stream password

The video stream has no password until you set one. The password is asked twice, not shown, and only its hash is stored on the camera:

```sh
akam rtsp-auth set          # also: status, disable (needs the admin key of key-2k)
```

**Without the assistant.** Create `rtsp.conf`: one line `user:akam:<HA1>`, where HA1 is the lowercase MD5 of `user:akam:password` (realm
`akam`). The password itself is never stored. From your computer, without the password appearing in your shell history:

```sh
printf 'Stream user: '; read -r U; printf 'Stream password: '; read -rs P; echo
H=$(printf '%s:akam:%s' "$U" "$P" | md5sum | cut -d' ' -f1)
printf '%s:akam:%s\n' "$U" "$H" | ssh -i KEY root@CAMERA_IP 'umask 077; cat > /config/akam/rtsp.conf'
unset P H
```

(`md5sum` is `md5 -r` on macOS.) The user is 1 to 32 characters. A file that is present but invalid refuses every request
(fail closed): delete it over SSH to go back to no password. ONVIF logs in with the same credentials, and **ONVIF will not
start without them**.

## 2. MQTT and Home Assistant

Home Assistant finds the 2K by MQTT discovery only: there is no dedicated integration and no broker on the camera. You need a broker on
your network (for example the Mosquitto add-on). With the assistant (the broker password is not shown and goes only in the signed
request; applies at the next restart of the app):

```sh
akam mqtt set               # broker address, port, user, password
akam mqtt status            # also: disable, forget
```

**Without the assistant.** Create the two files over SSH (the password is read from the terminal, not from a command line):

```sh
printf 'enabled=1\nhost=BROKER_ADDRESS\nport=1883\nuser=BROKER_USER\n' | ssh -i KEY root@CAMERA_IP 'umask 077; cat > /config/akam/mqtt'
printf 'Broker password: '; read -rs P; echo
printf '%s\n' "$P" | ssh -i KEY root@CAMERA_IP 'umask 077; cat > /config/akam/mqtt_pass'; unset P
```

Then restart the app. The camera connects out, in plain TCP (no TLS yet), and one Home Assistant device appears. Optional
keys, discovery entities and the topics, including the 2K-only `BASE/detections`: [MQTT](../../mqtt.md#lsc-rotatable-2k).
The file is strict: an unknown key or a bad value makes the whole file invalid and MQTT stays off.

## 3. ONVIF (pan/tilt in an NVR or app)

Off by default. Set a stream password first (section 1), then:

```sh
akam onvif enable [PORT]    # PORT 1024 to 65535, default 8000; also: disable, status
akam ntp set SERVER         # time sync (also off, status); read at the next restart of the app
akam audio mic on           # microphone track (also off); read at the next restart of the app
```

**Without the assistant.** Add `onvif=1` to `/config/akam/akam.conf` (optionally `onvif_port=8000`, the default) and restart the app.
ONVIF listens on TCP 8000 with WS-Discovery on UDP 3702. Details and limits: [ONVIF](../../onvif.md).

## 4. The options file `akam.conf`

Plain `key=value` lines, `#` comments. A bad line or an unknown key is logged and ignored (the default stays), so a typo
never stops the camera from streaming. Values are digits or the listed words. The keys you will use first:

| Key | Values (default) | Effect |
|---|---|---|
| `main_kbps`, `sub_kbps` | 64..8000 (2000), 32..1000 (800) | bit rate of `/stream0`, `/stream1` |
| `audio` | 0, 1 (0) | add a microphone track to both streams |
| `onvif`, `onvif_port` | 0, 1 (0); 1024..65535 (8000) | ONVIF |
| `track` | 0, 1 (0) | person tracking with the motor |
| `detect` | 0, 1 (1) | person detection on the NPU |
| `night`, `ir_led` | auto, day, night; auto, off | day/night mode, IR LEDs |
| `led` | auto, on, off (on) | the front LED |

The full list, with ranges and every tracking key, is the header of
[`own_app_run.sh`](../../../firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh); the tracking keys are
explained in [tracking](tracking.md), the picture in [ISP controls](isp-controls.md), the LEDs in [hal_light](hal-light.md).

## 5. The signed API (port 8092)

Nothing listens on 8092 until an API key exists. Create one over SSH, or let the assistant do it
(`akam key-2k --model lsc-rotatable-2k-ak3918av130 --ip CAMERA_IP`, the key stays on your PC, never shown):

```sh
ssh -i KEY root@CAMERA_IP 'sh /appfs/akam/akam_api_key.sh new --scope admin --name mytool'    # card-less (the normal case)
# with the first-install card inserted instead:  sh /mnt/sdcard/akam/akam_api_key.sh new ...
```

The key id and secret are printed once. Requests are HMAC-signed JSON over plain HTTP: how to sign, the settings registry
(LEDs, picture, video, PTZ, tracking, orientation), actions and the event feed are in the [2K control API](control-api.md).
Keep the camera on your LAN; the port has no TLS.

**From the assistant** (needs the key of `akam key-2k`; without it the assistant says so and points to `key-2k`): the Camera
options screen (menu item 3) lists the settings by group with their current values, edits them locally and sends them in
one `PATCH /settings` after a confirmation. The same from the command line:

```sh
akam led status off ir off              # front LED off, IR night LEDs off
akam picture brightness 60 flip on      # brightness is not saved (back to 50 at a restart)
akam video stream 0 bitrate 2000 fps 10 # main stream
akam hostname cam-salon                 # queued: the camera applies it within seconds
akam wifi country FR band 5             # asks first; 5 GHz needs a country
akam track on return stay
akam settings list                      # every setting of the catalogue with its value
akam settings get track.               # by key or prefix
akam settings set led.person 3:250:250:5000 track.band_pct 8
akam --dry-run settings set net.wifi_band 5   # prints the PATCH body, sends nothing
```

Values are checked like the firmware checks them (host name RFC 1123 label, 1..32; band 5 needs a country; QP and bitrate order;
ranges) and the camera's own refusal text is shown when it still says no. A result says applied now, queued (Wi-Fi and name),
needs a restart (the assistant never restarts or power-cycles the camera), or not saved. The two passwords (stream and MQTT) are
write-only rows: they are asked without echo, sent once in the signed request, shown as `***` in a rehearsal, never listed, returned, echoed
or logged by the camera. The API only queues them in RAM; the camera's supervisor validates them again and writes the files (section 1 and 2
files) within a second, only when their content changes, so nothing is written to the flash at run time. Needs an admin key. The assistant's commands were run against a fake of the API only; the same rows were written on the real 2K on
2026-11-23 with a curl-based signed helper.

## 6. At install time (`prepare-2k-sd`)

After the Wi-Fi and the cloud question, `prepare-2k-sd` asks whether to set up the optional services now (default No): microphone, a
stream password (user, password twice), ONVIF (only with a stream password), MQTT (broker, port, user, password) and time sync. The answers
become the files of this page on the first-install card (`akam.conf`, `mqtt`, `mqtt_pass`, `rtsp.conf` with the Digest hash only, `ntp`);
`seed-2k` copies them into `/config/akam` with the Wi-Fi and the key (a file already there is never overwritten). Every service stays off
if you answer No; change any of them later with the commands above.

## 7. Speaker, two-way audio and picture settings

*Status: the speaker beep and talk with `tools/akam-talk` over TCP were checked on the camera on 2026-11-23; volume and gain,
go2rtc and Frigate are not yet. Saturation, denoise and white balance are host-tested only. Flip and mirror were checked on the
camera on 2026-11-22 (colour, day/night and ceiling tracking not yet). The checks:
[speaker, talk and picture](../../hardware-test-checklists.md#2k-speaker-two-way-audio-and-picture-settings),
[orientation](../../hardware-test-checklists.md#2k-picture-orientation-flip-mirror-ceiling-mount).*

The 2K has the 1080p's speaker, two-way audio, saturation, denoise and white balance, under the **same setting names**. Set them in the
Camera options screen of the `akam` program (groups **Picture** and **Audio**), with `akam settings set KEY VALUE` (signed API), or at
install time: the `prepare-2k-sd` setup questions (the same ones as the services) ask about the speaker, two-way audio and the picture; the answers
are the file `settings` of the first-install card, which `seed-2k` copies to `/config/akam/settings` (an existing settings file is never overwritten).

| Setting | Values | Default | Notes |
|---|---|---|---|
| `audio.out_enable` | 0 / 1 | 0 | the speaker. **Off by default on the 2K** (the first checks are done, the rest is not). Turning it on starts it at once (no restart) |
| `audio.out_volume` | 0..100 | 100 | 0 mutes |
| `audio.out_gain` | 0..5 | 2 | hardware gain step of the amplifier (2 is the stock value) |
| `audio.out_amp` | auto, on, off | auto | amplifier policy; auto = on while there is sound. Not saved |
| `audio.talk_enable` | 0 / 1 | 1 | the ONVIF audio backchannel of the RTSP stream. It is offered only with the stream password on (section 1) and the speaker on |
| `audio.talk_duplex` | half, full | half | half silences the microphone track while speech plays on the speaker (stops echo) |
| `isp.saturation` | -50..50 | 0 | 0 = the camera's own; applied live and put back after a day/night switch |
| `isp.denoise` | 0..4 | 0 | 0 = the camera's own tables; N takes the temporal noise reduction of each light level from the level N steps higher (the 1080p does the same and defaults to 2) |
| `img.wb` | conf, manual | conf | conf = the automatic white balance; manual = fixed (day only). Not saved on the 2K: back to conf at a restart (the 1080p saves it) |

Read-only state: `audio.out_state` (`off`, `idle`, `talking`, `test`, `error`, `disabled_crash`) and `audio.talk_state` (`idle`, `active`). The action
`audio.beep` plays a one-second test beep (`POST /akam/v1/actions/audio.beep`).

**Talk.** From your computer: `akam talk` (your microphone, Ctrl-C stops), `akam talk test`, `akam say FILE`, `akam audio beep` (the speaker
must be on: `akam audio on`). Any other client that speaks the ONVIF backchannel over **TCP** works too (go2rtc, ffmpeg and Frigate use TCP by
default): the 2K server has no UDP. Nothing is offered without the stream password. One talker at a time; the speaker is then silent again
after two seconds. Home Assistant has no speaker entities on the 2K yet.

### Picture orientation (flip, mirror, ceiling)

`image.flip` (vertical), `image.mirror` (horizontal) and `image.ceiling` (both, for a camera mounted upside down) are API settings and
Home Assistant switches, off by default. Flip and mirror were checked on the camera on 2026-11-22; colour, day/night switches and ceiling
tracking are not yet checked. `akam picture flip on|off`, `mirror on|off`, `ceiling on|off`.

**If the speaker does not start.** `audio.out_state` says `error` (the playback device or a driver call failed: see the app log, lines `audio_out`) or
`disabled_crash`. A crash guard file `/config/akam/audio_out.trying` is written only when you turn the speaker on (`audio.out_enable` 0 to 1, a rare event), just before the first playback driver call of
that start, and removed five seconds later (or at once on a clean stop); if the camera ever rebooted inside that window, the guard is still there at the next start and
the speaker is skipped, so a kernel fault on the first run cannot loop the camera. Normal starts (power-up, a restart of the app by the supervisor or the watchdog) write nothing. After you have looked at the log, remove it over SSH
(`rm /config/akam/audio_out.trying`) and set `audio.out_enable` 1 again. That is the only flash write of the speaker besides the saved setting itself.
