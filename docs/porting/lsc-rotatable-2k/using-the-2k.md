# Using the 2K after the install

*What this page is for: the first things to do once your Rotatable 2K runs Akam from its flash: stream password, MQTT and
Home Assistant, ONVIF, the settings file, the signed API. Skip it if you only want to watch the video.*

The 2K is configured through small files in `/config/akam` (the jffs2 partition, kept across Akam updates), written over
SSH, and through the signed API on port 8092. The 1080p commands `rtsp-auth`, `mqtt`, `onvif`, `ntp`, `audio`, `key` and
`config` are **refused on a 2K** ("not yet supported for this model"): do not look for them. `led`, `picture`, `video`,
`hostname` and the Camera options screen of the `akam` program work on a 2K through the signed API (section 5), as do `track`,
`wifi` and `settings`. Everything here is host-tested; the checks that ran on the real camera are in
[testing status](../../testing-status.md) (the current build runs on the validated unit since 2026-11-22).

Log in with the key you gave `prepare-2k-sd` (`ssh -i KEY root@CAMERA_IP`). Files are small: the partition is 256 KiB. Each
file is written only when you change it; Akam never rewrites them while running.

| File in `/config/akam` | What it does | When a change applies |
|---|---|---|
| `rtsp.conf` | the stream password (Digest); absent = no password | at the next request |
| `mqtt`, `mqtt_pass` | MQTT and Home Assistant | next restart of the app |
| `akam2k.conf` | options of the app: bit rates, ONVIF, microphone, tracking, LEDs, ... | next restart of the app |
| `zones`, `ptz.conf`, `ntp`, `hostname`, `net` | detection zones, PTZ, clock sync, name, Wi-Fi band | see their pages |
| `settings`, `api_keys` | settings saved by the API (including flip, mirror, ceiling), API keys | written by the app |

To restart the app without a power cycle: `killall akam2k_rtsp` over SSH; the supervisor starts it again within a few
seconds (it gives up after 5 exits in 10 minutes). The `akam` program never reboots the camera.

## 1. Set the stream password

The video stream has no password until you create `rtsp.conf`: one line `user:akam:<HA1>`, where HA1 is the lowercase MD5 of
`user:akam:password` (realm `akam`). The password itself is never stored. From your computer, without the password
appearing in your shell history:

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

Home Assistant finds the 2K by MQTT discovery only: there is no dedicated integration and no broker on the camera. You need
a broker on your network (for example the Mosquitto add-on). Create the two files over SSH (the password is read from the
terminal, not from a command line):

```sh
printf 'enabled=1\nhost=BROKER_ADDRESS\nport=1883\nuser=BROKER_USER\n' | ssh -i KEY root@CAMERA_IP 'umask 077; cat > /config/akam/mqtt'
printf 'Broker password: '; read -rs P; echo
printf '%s\n' "$P" | ssh -i KEY root@CAMERA_IP 'umask 077; cat > /config/akam/mqtt_pass'; unset P
```

Then restart the app. The camera connects out, in plain TCP (no TLS yet), and one Home Assistant device appears. Optional
keys, discovery entities and the topics, including the 2K-only `BASE/detections`: [MQTT](../../mqtt.md#lsc-rotatable-2k).
The file is strict: an unknown key or a bad value makes the whole file invalid and MQTT stays off.

## 3. ONVIF (pan/tilt in an NVR or app)

Off by default. Set a stream password first (section 1), then add `onvif=1` to `/config/akam/akam2k.conf` (optionally
`onvif_port=8000`, the default) and restart the app. ONVIF listens on TCP 8000 with WS-Discovery on UDP 3702. Details and
limits: [ONVIF](../../onvif.md).

## 4. The options file `akam2k.conf`

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
ssh -i KEY root@CAMERA_IP 'sh /mnt/sdcard/akam/akam_api_key.sh new --scope admin --name mytool'
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
needs a restart (the assistant never restarts or power-cycles the camera), or not saved. No secret is shown or sent: the
registry has none, so the stream password and MQTT stay in `akam2k.conf` (sections 1 and 2). **Host-tested only**: this was run
against a fake of the API, not against a real 2K.

## Picture orientation (flip, mirror, ceiling)

`image.flip` (vertical), `image.mirror` (horizontal) and `image.ceiling` (both, for a camera mounted upside down) are API
settings and Home Assistant switches, off by default. They are host-tested only and **not yet verified on a camera**:
[checklist](../../hardware-test-checklists.md#2k-picture-orientation-flip-mirror-ceiling-mount).
